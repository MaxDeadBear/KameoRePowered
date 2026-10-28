"""Reader for Rare's CAFF 28.01.05.0031 archives (Kameo .lvl and .mdl files).

Layout, as the game's loader reads it (CheckVersion, AllocateSectionBlocks, ProcessTables, ProcessObjectFile):
  0x000  "CAFF" + version string
  0x014  checksum: ELF hash over the 0x180-byte header with this field zeroed
  0x018  symbol count
  0x01C  asset entry count
  0x020  byte-swap group count, 0x024 byte-swap offset count
  0x028  relocation group count, 0x02C relocation offset count
  0x034  offset of the table block inside the first section
  0x038  header size (0x180)
  0x03D  section count
  0x040  section entries, 40 bytes each: name[8], type, alignment log2, size, ..., compressed size
Each section follows the header as its own zlib stream, in table order.

The table block holds a name-blob size (with name offsets and the blob when non-zero), then the asset
entries (14 bytes: symbol, offset, size, section index, section type), byte-swap groups and offsets, and
relocation groups (source entry, target entry, count) followed by their offsets.  A relocated pointer is
stored as an offset into the target entry and becomes an address once the target is placed.

Writing lays each section's entries out again in their old order, each at an offset with the alignment its old one
had (capped at 4096), so an entry can change size; the table block follows the first section's entries, and each
section is padded to four bytes. Pointers are
entry-relative, so moving an entry needs nothing else.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

MAGIC = b"CAFF"
VERSION = b"28.01.05.0031"
HEADER_SIZE = 0x180


def _u32(buffer: bytes, offset: int) -> int:
    return struct.unpack_from(">I", buffer, offset)[0]


def _aligned(cursor: int, original_offset: int) -> int:
    """`cursor` rounded up to the alignment `original_offset` had: its lowest set bit, at most 4096."""
    alignment = min(original_offset & -original_offset, 4096) if original_offset else 4096
    return (cursor + alignment - 1) // alignment * alignment


def header_checksum(header: bytes) -> int:
    blank = bytearray(header[:HEADER_SIZE])
    blank[0x14:0x18] = b"\0\0\0\0"
    value = 0
    for byte in blank:
        value = ((value << 4) + (byte if byte < 0x80 else byte - 0x100)) & 0xFFFFFFFF
        top = value & 0xF0000000
        if top:
            value ^= (top >> 24) | top
    return value


@dataclass
class Section:
    name: str
    type: int
    alignment: int
    size: int
    compressed_size: int
    data: bytes = b""


@dataclass
class Entry:
    index: int
    symbol: int
    offset: int
    size: int
    section: int
    section_type: int

    def read(self, archive: "Caff") -> bytes:
        section = archive.sections[self.section - 1]
        return section.data[self.offset:self.offset + self.size]


@dataclass
class Relocation:
    source: int
    target: int
    offsets: list[int] = field(default_factory=list)


@dataclass
class Caff:
    header: bytes
    sections: list[Section]
    entries: list[Entry]
    swap_groups: list[tuple[int, int, int]]
    swap_offsets: list[int]
    relocations: list[Relocation]
    symbol_count: int

    @classmethod
    def load(cls, path: str) -> "Caff":
        with open(path, "rb") as handle:
            return cls.parse(handle.read())

    @classmethod
    def parse(cls, blob: bytes) -> "Caff":
        if blob[:4] != MAGIC or blob[4:4 + len(VERSION)] != VERSION:
            raise ValueError("not a CAFF 28.01.05.0031 archive")
        header = blob[:HEADER_SIZE]
        if header_checksum(header) != _u32(header, 0x14):
            raise ValueError("CAFF header checksum mismatch")
        sections = []
        cursor = _u32(header, 0x38)
        for i in range(header[0x3D]):
            base = 0x40 + 40 * i
            name = header[base:base + 8].rstrip(b"\0").decode("ascii")
            values = struct.unpack_from(">8I", header, base + 8)
            section = Section(name, values[0], values[1], values[2], values[7])
            if section.compressed_size:
                section.data = zlib.decompress(blob[cursor:cursor + section.compressed_size])
                cursor += section.compressed_size
            if len(section.data) != section.size:
                raise ValueError(f"section {name} unpacked to {len(section.data)} bytes, expected {section.size}")
            sections.append(section)

        table = sections[0].data
        cursor = _u32(header, 0x34)
        name_blob_size = _u32(table, cursor)
        cursor += 4
        if name_blob_size:
            cursor += 4 * _u32(header, 0x18) + name_blob_size
        entries = []
        for i in range(_u32(header, 0x1C)):
            symbol, offset, size, section, section_type = struct.unpack_from(">IIIBB", table, cursor)
            entries.append(Entry(i + 1, symbol, offset, size, section, section_type))
            cursor += 14
        swap_groups = [struct.unpack_from(">III", table, cursor + 12 * i) for i in range(_u32(header, 0x20))]
        cursor += 12 * len(swap_groups)
        swap_offsets = [_u32(table, cursor + 4 * i) for i in range(_u32(header, 0x24))]
        cursor += 4 * len(swap_offsets)
        groups = [struct.unpack_from(">III", table, cursor + 12 * i) for i in range(_u32(header, 0x28))]
        cursor += 12 * len(groups)
        relocations = []
        for source, target, count in groups:
            relocations.append(Relocation(source, target, [_u32(table, cursor + 4 * i) for i in range(count)]))
            cursor += 4 * count
        return cls(header, sections, entries, swap_groups, swap_offsets, relocations, _u32(header, 0x18))

    def write(self, replaced: dict[int, bytes] | None = None, level: int = 9) -> bytes:
        """The archive with the entries in `replaced` (by entry index) given new contents."""
        replaced = replaced or {}
        header = bytearray(self.header)
        table_offset = _u32(header, 0x34)
        table = self.sections[0].data[table_offset:]
        name_blob_size = _u32(table, 0)
        names_end = 4 + (4 * self.symbol_count + name_blob_size if name_blob_size else 0)
        layout: dict[int, int] = {}
        bodies: list[bytearray] = []
        for number, section in enumerate(self.sections, start=1):
            body = bytearray()
            for entry in sorted((e for e in self.entries if e.section == number), key=lambda e: e.offset):
                data = replaced.get(entry.index, entry.read(self))
                body += bytes(_aligned(len(body), entry.offset) - len(body))
                layout[entry.index] = len(body)
                body += data
            bodies.append(body)
        first = bodies[0]
        first += bytes(_aligned(len(first), table_offset) - len(first))
        struct.pack_into(">I", header, 0x34, len(first))
        first += table[:names_end]
        for entry in self.entries:
            size = len(replaced[entry.index]) if entry.index in replaced else entry.size
            first += struct.pack(">IIIBB", entry.symbol, layout[entry.index], size, entry.section, entry.section_type)
        for group in self.swap_groups:
            first += struct.pack(">III", *group)
        for offset in self.swap_offsets:
            first += struct.pack(">I", offset)
        for relocation in self.relocations:
            first += struct.pack(">III", relocation.source, relocation.target, len(relocation.offsets))
        for relocation in self.relocations:
            first += b"".join(struct.pack(">I", offset) for offset in relocation.offsets)
        # The counts, for an archive that gained entries or pointers.
        struct.pack_into(">II", header, 0x18, self.symbol_count, len(self.entries))
        struct.pack_into(">II", header, 0x28, len(self.relocations), sum(len(r.offsets) for r in self.relocations))
        packed = []
        for i, body in enumerate(bodies):
            body += bytes(-len(body) % 4)
            data = zlib.compress(bytes(body), level) if body else b""
            struct.pack_into(">I", header, 0x40 + 40 * i + 16, len(body))
            struct.pack_into(">I", header, 0x40 + 40 * i + 36, len(data))
            packed.append(data)
        struct.pack_into(">I", header, 0x14, header_checksum(bytes(header)))
        return bytes(header) + bytes(_u32(header, 0x38) - HEADER_SIZE) + b"".join(packed)

    def entries_for_symbol(self, symbol: int) -> list[Entry]:
        return [entry for entry in self.entries if entry.symbol == symbol]

    def pointers_from(self, entry: Entry) -> dict[int, tuple[Entry, int]]:
        """Pointer fields inside `entry`, mapped to the entry and offset each points at."""
        found = {}
        data = entry.read(self)
        for relocation in self.relocations:
            if relocation.source != entry.index:
                continue
            target = self.entries[relocation.target - 1]
            for offset in relocation.offsets:
                if offset + 4 <= len(data):
                    found[offset] = (target, _u32(data, offset))
        return found
