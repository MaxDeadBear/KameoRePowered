"""Image work for texture import, NumPy only so it runs inside Blender: reading PNGs, making mip levels, and
encoding the block formats the models use (DXT1, DXT3, DXT5, DXN) and the plain ones.

The block encoders fit each 4x4 block's endpoints along the principal axis of its colours (or the range of a single
channel) and give every texel the nearest of the colours those endpoints decode to, decoded exactly as texture.py
decodes them. That is a plain encoder, not an optimising one: a texture round-trips within a few levels per channel.
"""

from __future__ import annotations

import struct
import zlib

import numpy as np

from .texture import PNG_SIGNATURE, _alpha_blocks, _colour_blocks


def read_png(path: str) -> np.ndarray:
    """An 8- or 16-bit PNG (greyscale, RGB, palette, with or without alpha, not interlaced) as (h, w, 4) bytes."""
    with open(path, "rb") as handle:
        return read_png_bytes(handle.read(), path)


def read_png_bytes(data: bytes, path: str = "image") -> np.ndarray:
    """read_png for a PNG already in memory."""
    if data[:8] != PNG_SIGNATURE:
        raise ValueError(f"{path} is not a PNG")
    cursor, idat, palette, transparency = 8, bytearray(), None, None
    while cursor < len(data):
        length, kind = struct.unpack_from(">I4s", data, cursor)
        body = data[cursor + 8:cursor + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = np.frombuffer(body, np.uint8).reshape(-1, 3)
        elif kind == b"tRNS":
            transparency = np.frombuffer(body, np.uint8)
        elif kind == b"IDAT":
            idat += body
        cursor += 12 + length
    if interlace or depth not in (8, 16) and colour != 3:
        raise ValueError(f"{path}: interlaced or {depth}-bit PNGs are not supported; save it as an ordinary 8-bit PNG")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    bpp = channels * depth // 8 if colour != 3 else 1
    stride = (width * channels * depth + 7) // 8
    raw = np.frombuffer(zlib.decompress(bytes(idat)), np.uint8).reshape(height, stride + 1)
    rows = np.zeros((height, stride), np.uint8)
    previous = np.zeros(stride, np.int32)
    for y in range(height):
        kind, line = raw[y, 0], raw[y, 1:].astype(np.int32)
        if kind == 0:
            current = line
        elif kind == 2:
            current = (line + previous) & 255
        else:
            current = np.zeros(stride, np.int32)
            for x in range(stride):
                left = current[x - bpp] if x >= bpp else 0
                if kind == 1:
                    current[x] = (line[x] + left) & 255
                elif kind == 3:
                    current[x] = (line[x] + ((left + previous[x]) >> 1)) & 255
                else:
                    up_left = previous[x - bpp] if x >= bpp else 0
                    p = left + previous[x] - up_left
                    pa, pb, pc = abs(p - left), abs(p - previous[x]), abs(p - up_left)
                    predictor = left if pa <= pb and pa <= pc else previous[x] if pb <= pc else up_left
                    current[x] = (line[x] + predictor) & 255
        rows[y] = current
        previous = current
    if colour == 3:
        if depth != 8:
            bits = np.unpackbits(rows, axis=1)[:, :width * depth].reshape(height, width, depth)
            index = (bits * (1 << np.arange(depth - 1, -1, -1))).sum(axis=2)
        else:
            index = rows[:, :width]
        alpha = np.full(256, 255, np.uint8)
        if transparency is not None:
            alpha[:len(transparency)] = transparency
        return np.concatenate([palette[index], alpha[index][:, :, None]], axis=2)
    pixels = rows.reshape(height, width, channels * depth // 8)
    if depth == 16:
        pixels = pixels[:, :, 0::2]
    if channels == 1:
        return np.concatenate([np.repeat(pixels, 3, axis=2), np.full((height, width, 1), 255, np.uint8)], axis=2)
    if channels == 2:
        return np.concatenate([np.repeat(pixels[:, :, :1], 3, axis=2), pixels[:, :, 1:2]], axis=2)
    if channels == 3:
        return np.concatenate([pixels, np.full((height, width, 1), 255, np.uint8)], axis=2)
    return pixels.copy()


def resize(pixels: np.ndarray, width: int, height: int) -> np.ndarray:
    """A box-filtered (or, growing, nearest) resize, enough to fit an image to a texture's size."""
    h, w = pixels.shape[:2]
    ys = (np.arange(height) * h // height)
    xs = (np.arange(width) * w // width)
    if h % height == 0 and w % width == 0:
        fy, fx = h // height, w // width
        return pixels.reshape(height, fy, width, fx, -1).astype(np.float32).mean(axis=(1, 3)).round().astype(np.uint8)
    return pixels[ys][:, xs]


def half(pixels: np.ndarray) -> np.ndarray:
    """The next mip level: each 2x2 averaged (an odd last row or column dropped), a side of 1 kept."""
    p = pixels.astype(np.float32)
    if p.shape[0] > 1:
        h = p.shape[0] // 2 * 2
        p = (p[0:h:2] + p[1:h:2]) / 2
    if p.shape[1] > 1:
        w = p.shape[1] // 2 * 2
        p = (p[:, 0:w:2] + p[:, 1:w:2]) / 2
    return p.round().astype(np.uint8)


def _blocks(pixels: np.ndarray) -> np.ndarray:
    """An (h, w, c) image as (blocks, 16, c) texels of 4x4 blocks, edges repeated to whole blocks."""
    h, w, c = pixels.shape
    bh, bw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    padded = np.pad(pixels, ((0, bh - h), (0, bw - w), (0, 0)), mode="edge")
    return padded.reshape(bh // 4, 4, bw // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def _encode_colour(texels: np.ndarray, punch_through: bool) -> np.ndarray:
    """BC1 colour halves, (n, 8) bytes, for (n, 16, 4) texels. With punch_through, a block holding texels with alpha
    below 128 uses the three-colour mode, those texels taking the transparent index."""
    rgb = texels[:, :, :3].astype(np.float32)
    mean = rgb.mean(axis=1, keepdims=True)
    centred = rgb - mean
    cov = np.einsum("nki,nkj->nij", centred, centred)
    axis = np.ones((len(rgb), 3), np.float32)
    for _ in range(8):
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum("nki,ni->nk", centred, axis)
    lo = mean[:, 0] + axis * t.min(axis=1, keepdims=True)
    hi = mean[:, 0] + axis * t.max(axis=1, keepdims=True)

    def to565(c: np.ndarray) -> np.ndarray:
        c = np.clip(c.round(), 0, 255).astype(np.int32)
        return ((c[:, 0] >> 3) << 11) | ((c[:, 1] >> 2) << 5) | (c[:, 2] >> 3)

    c0, c1 = to565(hi), to565(lo)
    transparent = (texels[:, :, 3] < 128) if punch_through else np.zeros(texels.shape[:2], bool)
    three = transparent.any(axis=1)
    # Four-colour mode wants colour 0 above colour 1, three-colour mode at or below it.
    swap = np.where(three, c0 > c1, c0 < c1)
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    four_equal = (~three) & (c0 == c1)
    blocks = np.zeros((len(rgb), 8), np.uint8)
    blocks[:, 0], blocks[:, 1], blocks[:, 2], blocks[:, 3] = c0 & 255, c0 >> 8, c1 & 255, c1 >> 8
    # The four colours exactly as the decoder makes them: texel 0 of a block whose indices are all k.
    probe = np.zeros((len(rgb), 8), np.uint8)
    probe[:, :4] = blocks[:, :4]
    colours = []
    for index in range(4):
        probe[:, 4:] = np.uint8(index * 0x55)
        colours.append(_colour_blocks(probe, four_colour=False)[:, 0, :])
    palette = np.stack(colours, axis=1).astype(np.float32)  # (n, 4, 4)
    distance = ((rgb[:, :, None, :] - palette[:, None, :, :3]) ** 2).sum(axis=3)  # (n, 16, 4)
    distance[:, :, 3] = np.where(three[:, None], np.inf, distance[:, :, 3])
    index = distance.argmin(axis=2)
    index = np.where(transparent, 3, index)
    index = np.where(four_equal[:, None], 0, index)
    bits = (index.astype(np.uint32) << (2 * np.arange(16, dtype=np.uint32))).sum(axis=1, dtype=np.uint64).astype(np.uint32)
    for k in range(4):
        blocks[:, 4 + k] = (bits >> np.uint32(8 * k)) & 255
    return blocks


def _encode_single(values: np.ndarray) -> np.ndarray:
    """BC4 halves, (n, 8) bytes, for (n, 16) channel values, in the eight-value mode."""
    v = values.astype(np.int32)
    a0, a1 = v.max(axis=1), v.min(axis=1)
    flat = a0 == a1
    a1 = np.where(flat & (a0 > 0), a0 - 1, a1)
    a0 = np.where(flat & (a0 == 0), 1, a0)
    blocks = np.zeros((len(v), 8), np.uint8)
    blocks[:, 0], blocks[:, 1] = a0, a1
    # Value k of the eight-value palette, as the decoder makes it: texel 0 of a block whose first index is k.
    levels = []
    for k in range(8):
        packed = np.zeros((len(v), 8), np.uint8)
        packed[:, :2] = blocks[:, :2]
        packed[:, 2] = k
        levels.append(_alpha_blocks(packed)[:, 0])
    palette = np.stack(levels, axis=1)
    index = np.abs(v[:, :, None] - palette[:, None, :]).argmin(axis=2).astype(np.uint64)
    bits = (index << (np.uint64(3) * np.arange(16, dtype=np.uint64))).sum(axis=1, dtype=np.uint64)
    for k in range(6):
        blocks[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return blocks


def encode(pixels: np.ndarray, name: str) -> bytes:
    """An (h, w, 4) image in one of texture.py's formats, rows of blocks in order, little-endian as decoded."""
    if name in ("DXT1", "DXT3", "DXT5", "DXN"):
        texels = _blocks(pixels)
        if name == "DXT1":
            out = _encode_colour(texels, punch_through=True)
        elif name == "DXT3":
            alpha = (texels[:, :, 3].astype(np.int32) + 8) // 17
            packed = (alpha[:, 0::2] | (alpha[:, 1::2] << 4)).astype(np.uint8)
            out = np.concatenate([packed, _encode_colour(texels, punch_through=False)], axis=1)
        elif name == "DXT5":
            out = np.concatenate([_encode_single(texels[:, :, 3]), _encode_colour(texels, punch_through=False)], axis=1)
        else:
            out = np.concatenate([_encode_single(texels[:, :, 0]), _encode_single(texels[:, :, 1])], axis=1)
        return out.tobytes()
    if name == "8888":
        return np.ascontiguousarray(pixels[:, :, [2, 1, 0, 3]]).tobytes()
    if name == "565":
        p = pixels.astype(np.int32)
        c = ((p[:, :, 0] * 31 + 127) // 255) | (((p[:, :, 1] * 63 + 127) // 255) << 5) | (((p[:, :, 2] * 31 + 127) // 255) << 11)
        return c.astype("<u2").tobytes()
    if name == "88":
        return np.ascontiguousarray(pixels[:, :, [0, 3]]).tobytes()
    return np.ascontiguousarray(pixels[:, :, 0]).tobytes()
