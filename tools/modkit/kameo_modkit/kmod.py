"""Writes Kameo RePowered mod packages (.kmod), as the launcher's kmod.cpp reads them.

A package holds each changed archive as the difference from the shipped file, so it carries none of the game's own
data: the new header, then per section its new size and the runs of bytes that changed (changed bytes fewer than 64
apart are joined into one run). The launcher checks the shipped file against its size and FNV-1a hash, applies the
runs to its unpacked sections, checks the result against the hash the package records, and packs it again.

Layout, little-endian: "KMOD", version 1, the body's size, then the body as a zlib stream:
  name, author, version, description (each a u32 length and the text), file count; per file: name, base size (u64),
  base hash (u64), result hash (u64), header (u32 length, 0x180 bytes), section count; per section: size, run count;
  per run: offset, bytes (u32 length and the bytes).
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

from .caff import Caff

VERSION = 1
RUN_GAP = 64


def fnv1a(data: bytes, value: int = 1469598103934665603) -> int:
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def _hash_sections(sections: list[bytes]) -> int:
    value = 1469598103934665603
    for data in sections:
        value = fnv1a(data, value)
        value = ((value ^ 0xFF) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


@dataclass
class FilePatch:
    name: str
    base_size: int
    base_hash: int
    result_hash: int
    header: bytes
    sections: list[tuple[int, list[tuple[int, bytes]]]] = field(default_factory=list)


def _runs(before: bytes, after: bytes) -> list[tuple[int, bytes]]:
    """kmod::diff's runs: bytes of `after` that differ from `before`, gaps under RUN_GAP joined."""
    runs = []
    i, size, old = 0, len(after), len(before)
    while i < size:
        if i < old and before[i] == after[i]:
            i += 1
            continue
        end, same = i + 1, 0
        while end < size and same < RUN_GAP:
            same = same + 1 if end < old and before[end] == after[end] else 0
            end += 1
        end -= same
        runs.append((i, after[i:end]))
        i = end
    return runs


def diff(name: str, original: bytes, edited: bytes) -> FilePatch:
    before, after = Caff.parse(original), Caff.parse(edited)
    if len(before.sections) != len(after.sections):
        raise ValueError("the archives have different sections")
    patch = FilePatch(name, len(original), fnv1a(original), _hash_sections([s.data for s in after.sections]), edited[:0x180])
    for old, new in zip(before.sections, after.sections):
        patch.sections.append((len(new.data), _runs(old.data, new.data)))
    return patch


def write(files: list[FilePatch], name: str, author: str = "", version: str = "", description: str = "") -> bytes:
    body = bytearray()

    def text(value: str | bytes) -> None:
        data = value.encode("utf-8") if isinstance(value, str) else value
        body.extend(struct.pack("<I", len(data)) + data)

    for value in (name, author, version, description):
        text(value)
    body += struct.pack("<I", len(files))
    for f in files:
        text(f.name)
        body += struct.pack("<QQQ", f.base_size, f.base_hash, f.result_hash)
        text(f.header)
        body += struct.pack("<I", len(f.sections))
        for size, runs in f.sections:
            body += struct.pack("<II", size, len(runs))
            for offset, data in runs:
                body += struct.pack("<I", offset)
                text(data)
    return b"KMOD" + struct.pack("<II", VERSION, len(body)) + zlib.compress(bytes(body), 9)
