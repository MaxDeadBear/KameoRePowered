"""Reads the game's animations: the clips a character plays, as per-joint rotations, translations and scales.

An `animation` entry (animDecompress, the QUAT_BITSTREAM codec), big-endian:
  +28 its length in seconds, +32 joint count (u16), +34 frame count (u16), +64 records per frame (a float: 1 for a
  record every frame, 0.5 for one every second frame; interpCalculateInterpolationFrames), +36 the channel descriptor,
  whose +24 is the bitstream header:
    +4 per-channel scales (floats), +8 per-channel defaults, +12 the mask bytes, +16 the base values (s16), +20 the
    bit widths (a nibble each, low nibble first), +24 and +26 two lengths that lead from the base values to the frame
    records, +28 the size of a frame record in bytes, +30 the channel count, +32 a flag for 32-bit base values
A joint has nine channels: quaternion x y z (w is what makes it a unit quaternion, and positive), then a translation
added to the joint's own (skeleton.py), then scale. Clips with a tenth channel carry it in a row of its own; it is
not read here.
  The mask bytes say, joint by joint, which channels move: the first byte covers the rotation and its low two bits
  say whether bytes for the translation and the scale follow. In each, bits 0x80 0x40 0x20 mark a channel that has a
  value, and 0x10 0x08 0x04 that the value is one s16 for the whole clip; otherwise it is an s16 base plus, every
  frame, a number of `width` bits. A channel with no value keeps its default.
  A record is those per-frame numbers packed one after another, least significant bit first out of big-endian
  32-bit words. There are ceil((frames - 1) * step + 1) records; frame f sits at record f * step, and between two
  records the game turns the shorter way from one rotation to the other (quatSlerp) and blends the rest.
Track i drives joint i of the skeleton: a clip has no names. All the game's clips are 60 frames a second.

A `kameoanimblock` entry lists a character type's clips: +32 their count, +36 their ids (u32 each, hashes of names the
game asks for), +44 pointers to the animation entries.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

from .caff import Caff, Entry
from .graph import Ref, Resolver

FRAMES_PER_SECOND = 60


@dataclass
class Clip:
    entry: int          # the animation's entry index in its archive
    ident: int          # the id the game plays it by, 0 when no block lists it
    joints: int
    frames: int
    seconds: float


@dataclass
class Motion:
    rotation: np.ndarray     # (frame, joint, 4): x y z w
    translation: np.ndarray  # (frame, joint, 3), added to the joint's own translation
    scale: np.ndarray        # (frame, joint, 3)


def list_clips(resolver: Resolver, joints: int | None = None) -> list[Clip]:
    """The archive's clips, those of `joints` joints when given, in the order their character's block lists them."""
    archive = resolver.archive
    idents: dict[int, int] = {}
    order: list[int] = []
    for entry in archive.entries:
        if resolver.type_name(entry) != "kameoanimblock":
            continue
        block = Ref(entry, 0)
        ids, clips = resolver.pointer(block, 36), resolver.pointer(block, 44)
        for i in range(resolver.u32(block, 32) if ids and clips else 0):
            clip = resolver.pointer(clips, 4 * i)
            if clip is not None and clip.entry.index not in idents:
                idents[clip.entry.index] = resolver.u32(ids, 4 * i)
                order.append(clip.entry.index)
    listed = set(order)
    order += [e.index for e in archive.entries if resolver.type_name(e) == "animation" and e.index not in listed]
    out = []
    for index in order:
        entry = archive.entries[index - 1]
        if resolver.type_name(entry) != "animation":
            continue
        data = resolver.data(entry)
        count, frames = struct.unpack_from(">HH", data, 32)
        if joints is None or count == joints:
            out.append(Clip(index, idents.get(index, 0), count, frames, struct.unpack_from(">f", data, 28)[0]))
    return out


def read_motion(resolver: Resolver, entry: Entry) -> Motion:
    data = resolver.data(entry)

    def u16(offset: int) -> int:
        return struct.unpack_from(">H", data, offset)[0]

    def u32(offset: int) -> int:
        return struct.unpack_from(">I", data, offset)[0]

    if resolver.type_name(entry) != "animation":
        raise ValueError("not an animation")
    joints, frames = u16(32), u16(34)
    header = u32(u32(36) + 24)
    scales_at, defaults_at, mask, raw, widths = (u32(header + 4 * i) for i in (1, 2, 3, 4, 5))
    first = raw + u16(header + 24) + u16(header + 26)
    stride, channels = u16(header + 28), u16(header + 30)
    if data[header + 32]:
        raise ValueError("this clip stores 32-bit values, which is not supported")
    scale = struct.unpack_from(f">{channels}f", data, scales_at)
    default = struct.unpack_from(f">{channels}f", data, defaults_at)

    # One pass over the masks: per joint channel, a constant, or where its bits sit in a frame record.
    constant = np.zeros((joints, 9), np.float32)
    moving: list[tuple[int, int, int, int, int]] = []  # joint, channel, base, first bit, bits
    bit = 0
    high = False

    def group(byte: int, channel: int, joint: int) -> None:
        nonlocal raw, widths, high, bit
        for k in range(3):
            ch = channel + k
            if ch >= channels:
                return
            if not byte & (0x80 >> k):
                if ch < 9:
                    constant[joint, ch] = default[ch]
                continue
            base = struct.unpack_from(">h", data, raw)[0]
            raw += 2
            if byte & (0x10 >> k):
                if ch < 9:
                    constant[joint, ch] = base * scale[ch]
                continue
            width = ((data[widths] >> (4 if high else 0)) & 0xF) + 1
            if high:
                widths += 1
            high = not high
            if ch < 9:
                moving.append((joint, ch, base, bit, width))
            bit += width

    def defaults(low: int, top: int, joint: int) -> None:
        for ch in range(low, min(top, channels, 9)):
            constant[joint, ch] = default[ch]

    for joint in range(joints):
        for row in range(0, channels, 9):
            byte = data[mask]
            follows = byte & 3
            group(byte, row, joint)
            if follows == 0:
                defaults(row + 3, row + 9, joint)
            elif follows == 1:
                defaults(row + 3, row + 6, joint)
                mask += 1
                group(data[mask], row + 6, joint)
            elif follows == 2:
                mask += 1
                group(data[mask], row + 3, joint)
                defaults(row + 6, row + 9, joint)
            else:
                mask += 1
                group(data[mask], row + 3, joint)
                mask += 1
                group(data[mask], row + 6, joint)
            mask += 1

    step = struct.unpack_from(">f", data, 64)[0]
    if not 0.0 < step <= 1.0:
        step = 1.0
    records = max(1, int(np.ceil((frames - 1) * step + 1 - 1e-6)))
    values = np.repeat(constant[None, :, :], records, axis=0)
    if moving and frames:
        padded = data + bytes(-len(data) % 4)
        # Least significant bit first out of big-endian words: the words' bytes reversed, then bits low to high.
        bits = np.unpackbits(np.frombuffer(padded, ">u4").astype("<u4").view(np.uint8), bitorder="little")
        starts = (first + stride * np.arange(records, dtype=np.int64)) * 8
        for joint, ch, base, at, width in moving:
            index = starts[:, None] + at + np.arange(width)
            number = (bits[index].astype(np.int64) << np.arange(width)).sum(axis=1)
            values[:, joint, ch] = (number + base) * scale[ch]
    xyz = values[:, :, 0:3]
    w = np.sqrt(np.clip(1.0 - (xyz * xyz).sum(axis=2), 0.0, 1.0))
    rotation = np.concatenate([xyz, w[:, :, None]], axis=2)
    translation, size = values[:, :, 3:6], values[:, :, 6:9]
    if records == frames or frames <= 1:
        return Motion(rotation, translation.copy(), size.copy())
    # Frames between records.
    at = np.arange(frames) * step
    low = np.minimum(at.astype(np.int64), records - 1)
    high = np.minimum(low + 1, records - 1)
    part = (at - low)[:, None, None]
    a, b = rotation[low], rotation[high].copy()
    b[(a * b).sum(axis=2) < 0.0] *= -1.0
    angle = np.arccos(np.clip((a * b).sum(axis=2), -1.0, 1.0))[:, :, None]
    near = angle < 1e-4
    sine = np.where(near, 1.0, np.sin(angle))
    turned = np.where(near, a + (b - a) * part, (np.sin((1.0 - part) * angle) * a + np.sin(part * angle) * b) / sine)
    turned /= np.linalg.norm(turned, axis=2, keepdims=True)
    return Motion(turned.astype(np.float32), (translation[low] + (translation[high] - translation[low]) * part).astype(np.float32),
                  (size[low] + (size[high] - size[low]) * part).astype(np.float32))


def load(path: str) -> tuple[Caff, Resolver]:
    archive = Caff.load(path)
    return archive, Resolver(archive)
