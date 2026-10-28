"""Decodes `texture` assets to images.

A texture asset (texInitTexture) holds its D3DFORMAT at +24, kind at +28 (0 2D, 2 cube, 3 volume, 4 array),
width and height (u16) at +36/+38, the base level's data at +40 (in .gpu), level count (u8) at +44 and
depth/array size at +52.  The format's low six bits are the GPU texture format and the next two its endian
swap; the base level is stored in rows padded to 32 blocks, as the Xenos texture layout does.

Decoding is NumPy only (Blender ships NumPy but not Pillow), matching Pillow's BCn decoder: 5:6:5 colours widened
by repeating their top bits, DXT1 blocks with colour 0 <= colour 1 in three-colour mode with transparent black.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

import numpy as np

from .graph import Ref, Resolver

KIND_2D, KIND_CUBE, KIND_VOLUME, KIND_ARRAY = 0, 2, 3, 4
PNG_SIGNATURE = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
TILE_BLOCKS = 32

# GPU texture format -> (name, block width/height, bytes per block, DDS FourCC or None for uncompressed)
FORMATS = {
    18: ("DXT1", 4, 8, b"DXT1"),
    19: ("DXT3", 4, 16, b"DXT3"),
    20: ("DXT5", 4, 16, b"DXT5"),
    49: ("DXN", 4, 16, b"ATI2"),
    6: ("8888", 1, 4, None),
    4: ("565", 1, 2, None),
    2: ("8", 1, 1, None),
    10: ("88", 1, 2, None),
}


@dataclass
class Texture:
    ref: Ref
    format: int
    kind: int
    width: int
    height: int
    levels: int
    data: Ref | None

    @property
    def gpu_format(self) -> int:
        return self.format & 0x3F

    @property
    def endian(self) -> int:
        return (self.format >> 6) & 0x3

    @property
    def format_name(self) -> str:
        return FORMATS.get(self.gpu_format, (f"format{self.gpu_format}",))[0]


def read_texture(resolver: Resolver, ref: Ref) -> Texture:
    return Texture(ref, resolver.u32(ref, 24), resolver.u32(ref, 28), resolver.u16(ref, 36), resolver.u16(ref, 38), resolver.u8(ref, 44), resolver.pointer(ref, 40))


def _swap(data: bytes, endian: int) -> bytes:
    if endian == 1:
        swapped = bytearray(data)
        swapped[0::2], swapped[1::2] = data[1::2], data[0::2]
        return bytes(swapped)
    if endian == 2:
        count = len(data) // 4
        return struct.pack("<%dI" % count, *struct.unpack(">%dI" % count, data[:count * 4]))
    return data


def tiled_offset_2d(x: int, y: int, width: int, log2_bpp: int) -> int:
    """XGAddress2DTiledOffset: the block index of (x, y) in a tiled surface, as the console stores it."""
    aligned_width = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log2_bpp
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)
    return ((((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log2_bpp)


def base_level_bytes(resolver: Resolver, texture: Texture) -> bytes:
    """The base level, untiled and unpadded, in the GPU's byte order swapped to little endian."""
    _, block, block_bytes, _ = FORMATS[texture.gpu_format]
    blocks_x = (texture.width + block - 1) // block
    blocks_y = (texture.height + block - 1) // block
    source = resolver.data(texture.data.entry)
    base = texture.data.offset
    out = bytearray(blocks_x * blocks_y * block_bytes)
    if texture.format & 0x100:
        log2_bpp = block_bytes.bit_length() - 1
        for y in range(blocks_y):
            for x in range(blocks_x):
                start = base + tiled_offset_2d(x, y, blocks_x, log2_bpp) * block_bytes
                dest = (y * blocks_x + x) * block_bytes
                out[dest:dest + block_bytes] = source[start:start + block_bytes]
    else:
        pitch = ((blocks_x + TILE_BLOCKS - 1) // TILE_BLOCKS) * TILE_BLOCKS
        for y in range(blocks_y):
            start = base + y * pitch * block_bytes
            dest = y * blocks_x * block_bytes
            out[dest:dest + blocks_x * block_bytes] = source[start:start + blocks_x * block_bytes]
    return _swap(bytes(out), texture.endian)


def _expand565(c: np.ndarray) -> np.ndarray:
    """5:6:5 words to RGB bytes, (..., 3)."""
    r = (c >> 8) & 0xF8
    g = (c >> 3) & 0xFC
    b = (c << 3) & 0xF8
    return np.stack([r | (r >> 5), g | (g >> 6), b | (b >> 5)], axis=-1).astype(np.int32)


def _colour_blocks(blocks: np.ndarray, four_colour: bool) -> np.ndarray:
    """BC1 colour halves, (n, 8) bytes, to (n, 16, 4) RGBA."""
    c0 = blocks[:, 0].astype(np.int32) | (blocks[:, 1].astype(np.int32) << 8)
    c1 = blocks[:, 2].astype(np.int32) | (blocks[:, 3].astype(np.int32) << 8)
    p0, p1 = _expand565(c0), _expand565(c1)
    opaque = np.full(len(blocks), 255, np.int32)
    four = np.ones(len(blocks), bool) if four_colour else c0 > c1
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    a3 = np.where(four, 255, 0)
    palette = np.stack([np.concatenate([p, a[:, None]], axis=1) for p, a in ((p0, opaque), (p1, opaque), (p2, opaque), (p3, a3))], axis=1)
    bits = blocks[:, 4].astype(np.uint32) | (blocks[:, 5].astype(np.uint32) << 8) | (blocks[:, 6].astype(np.uint32) << 16) | (blocks[:, 7].astype(np.uint32) << 24)
    index = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    return np.take_along_axis(palette, index[:, :, None].astype(np.int64), axis=1)


def _alpha_blocks(blocks: np.ndarray) -> np.ndarray:
    """BC4 halves, (n, 8) bytes, to (n, 16) values."""
    a0 = blocks[:, 0].astype(np.int32)
    a1 = blocks[:, 1].astype(np.int32)
    i = np.arange(1, 7, dtype=np.int32)
    eight = np.concatenate([a0[:, None], a1[:, None], ((7 - i[:6]) * a0[:, None] + i[:6] * a1[:, None]) // 7], axis=1)
    j = np.arange(1, 5, dtype=np.int32)
    six = np.concatenate([a0[:, None], a1[:, None], ((5 - j) * a0[:, None] + j * a1[:, None]) // 5, np.zeros((len(blocks), 1), np.int32), np.full((len(blocks), 1), 255, np.int32)], axis=1)
    palette = np.where((a0 > a1)[:, None], eight, six)
    bits = np.zeros(len(blocks), np.uint64)
    for k in range(6):
        bits |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    index = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(palette, index.astype(np.int64), axis=1)


def _untile_blocks(texels: np.ndarray, width: int, height: int) -> np.ndarray:
    """(blocks_y * blocks_x, 16, c) texels of 4x4 blocks to an (height, width, c) image."""
    blocks_x, blocks_y = (width + 3) // 4, (height + 3) // 4
    image = texels.reshape(blocks_y, blocks_x, 4, 4, -1).transpose(0, 2, 1, 3, 4).reshape(blocks_y * 4, blocks_x * 4, -1)
    return image[:height, :width]


def to_pixels(resolver: Resolver, texture: Texture) -> np.ndarray:
    """The base level as an (height, width, 4) RGBA byte array."""
    if texture.gpu_format not in FORMATS or texture.data is None:
        raise ValueError(f"unsupported texture format {texture.format:08X}")
    payload = np.frombuffer(base_level_bytes(resolver, texture), np.uint8)
    w, h = texture.width, texture.height
    name = texture.format_name
    if name in ("DXT1", "DXT3", "DXT5", "DXN"):
        blocks = payload.reshape(-1, 8 if name == "DXT1" else 16)
        if name == "DXT1":
            texels = _colour_blocks(blocks, four_colour=False)
        elif name == "DXT3":
            texels = _colour_blocks(blocks[:, 8:], four_colour=True)
            nibbles = np.stack([blocks[:, :8] & 15, blocks[:, :8] >> 4], axis=2).reshape(-1, 16).astype(np.int32)
            texels[:, :, 3] = nibbles * 17
        elif name == "DXT5":
            texels = _colour_blocks(blocks[:, 8:], four_colour=True)
            texels[:, :, 3] = _alpha_blocks(blocks[:, :8])
        else:
            red, green = _alpha_blocks(blocks[:, :8]), _alpha_blocks(blocks[:, 8:])
            texels = np.stack([red, green, np.zeros_like(red), np.full_like(red, 255)], axis=2)
        return _untile_blocks(texels, w, h).astype(np.uint8)
    if name == "8888":
        # Little-endian ARGB words after the swap: bytes B, G, R, A.
        bgra = payload[:w * h * 4].reshape(h, w, 4)
        return bgra[:, :, [2, 1, 0, 3]].copy()
    if name == "565":
        c = payload[:w * h * 2].view("<u2").reshape(h, w).astype(np.int32)
        # Pillow's "BGR;16": red in the low five bits.
        rgb = np.stack([(c & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, ((c >> 11) & 31) * 255 // 31], axis=-1)
        return np.concatenate([rgb, np.full((h, w, 1), 255)], axis=2).astype(np.uint8)
    if name == "88":
        la = payload[:w * h * 2].reshape(h, w, 2)
        return np.stack([la[:, :, 0]] * 3 + [la[:, :, 1]], axis=2)
    l8 = payload[:w * h].reshape(h, w)
    return np.stack([l8] * 3 + [np.full_like(l8, 255)], axis=2)


def write_png(path: str, pixels: np.ndarray) -> None:
    """An (height, width, 3 or 4) byte array as an 8-bit RGB or RGBA PNG."""
    height, width, channels = pixels.shape
    rows = np.concatenate([np.zeros((height, 1), np.uint8), np.ascontiguousarray(pixels, np.uint8).reshape(height, width * channels)], axis=1)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6 if channels == 4 else 2, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows.tobytes(), 6)) + chunk(b"IEND", b""))


def _tiled_offsets(blocks_x: int, blocks_y: int, log2_bpp: int) -> np.ndarray:
    """tiled_offset_2d for every block of a level, (blocks_y, blocks_x)."""
    y, x = np.meshgrid(np.arange(blocks_y, dtype=np.int64), np.arange(blocks_x, dtype=np.int64), indexing="ij")
    aligned_width = (blocks_x + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log2_bpp
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log2_bpp


def first_packed_level(width: int, height: int) -> int:
    """The first mip level Xenos packs into the shared tail tile: the first with a side of 16 or less."""
    level = 0
    while max(width >> level, 1) > 16 and max(height >> level, 1) > 16:
        level += 1
    return level


def build_texture(pixels: np.ndarray, texture: Texture, levels: int) -> bytes:
    """A texture's data for an (h, w, 4) image in its format: each level down to the packed tail encoded, tiled at its
    pitch padded to 32 blocks and starting on its own 4 KB page, as the levels the game ships are; the tail itself is
    left empty (this port's renderer stops before it, at the first level with a side of 16 or less)."""
    from .encode import encode, half

    name, block, block_bytes, _ = FORMATS[texture.gpu_format]
    if not texture.format & 0x100:
        raise ValueError(f"{name} texture is not tiled; only tiled textures can be replaced")
    height, width = pixels.shape[:2]
    log2_bpp = block_bytes.bit_length() - 1
    out = bytearray()
    level_pixels = pixels
    for level in range(min(levels, first_packed_level(width, height))):
        if level:
            level_pixels = half(level_pixels)
        lh, lw = level_pixels.shape[:2]
        blocks_x, blocks_y = (lw + block - 1) // block, (lh + block - 1) // block
        encoded = np.frombuffer(encode(level_pixels, name), np.uint8).reshape(blocks_y, blocks_x, block_bytes)
        region = np.zeros((((blocks_x + 31) & ~31) * ((blocks_y + 31) & ~31), block_bytes), np.uint8)
        region[_tiled_offsets(blocks_x, blocks_y, log2_bpp).reshape(-1)] = encoded.reshape(-1, block_bytes)
        out += _swap(region.tobytes(), texture.endian)
        out += bytes(-len(out) % 4096)
    if levels > first_packed_level(width, height):
        out += bytes((32 * 32 * block_bytes + 4095) // 4096 * 4096)
    return bytes(out)
