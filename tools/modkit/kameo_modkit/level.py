"""Whole-level helpers: a level's name, and copying a level under a new one.

The game loads a level by name (LevelChange): the archive is the ELF hash of the name in hex, .lvl, and once it is
loaded the level object is looked up in it by the same hash. So a copy of a level under a new name is the archive with
its level object renamed.

kameoDatabase (the archive's first entry) +0x24 points at its object table:
  +0  object count   +4 records (12 bytes each: name pointer, 0, data pointer)   +8 a second per-object table
  +12 the hash index (12 bytes each: ELF hash of the name, 0, record pointer), sorted by hash; the game binary-searches it
  +16 the name blob (NUL-terminated names)
The level object is the one named after the level (its class is the level's own "<Level>.dll", id 0x72CCBC).
"""

from __future__ import annotations

import struct

from .caff import Caff, Entry


def elf_hash(name: str) -> int:
    """The game's string hash (signed chars), as level and model file names use."""
    value = 0
    for byte in name.encode("ascii"):
        value = ((value << 4) + (byte - 256 if byte >= 128 else byte)) & 0xFFFFFFFF
        top = value & 0xF0000000
        if top:
            value ^= (top >> 24) | top
    return value


def level_file_name(name: str) -> str:
    return "%08X.lvl" % elf_hash(name)


def database_entry(archive: Caff) -> Entry:
    return next(e for e in archive.entries if e.read(archive).startswith(b"kameoDatabase"))


def _string(data: bytes, at: int) -> str:
    return data[at:data.index(b"\0", at)].decode("ascii", "replace")


def object_names(archive: Caff) -> list[str]:
    data = database_entry(archive).read(archive)
    table = struct.unpack_from(">I", data, 0x24)[0]
    count, records = struct.unpack_from(">II", data, table)
    return [_string(data, struct.unpack_from(">I", data, records + 12 * i)[0]) for i in range(count)]


def rename_level(archive: Caff, old: str, new: str) -> bytes:
    """kameoDatabase with the level object `old` renamed `new` (at most 31 characters, LevelChange's buffer) and the
    hash index re-sorted. Returns the entry's new bytes for Caff.write; its pointers stay where they were."""
    if not 0 < len(new) <= 31:
        raise ValueError("a level name has 1 to 31 characters")
    entry = database_entry(archive)
    data = bytearray(entry.read(archive))
    table = struct.unpack_from(">I", data, 0x24)[0]
    count, records, _, index, _ = struct.unpack_from(">5I", data, table)
    record = next((records + 12 * i for i in range(count) if _string(data, struct.unpack_from(">I", data, records + 12 * i)[0]) == old), None)
    if record is None:
        raise ValueError(f"no object named {old!r} in the level")
    name_at = struct.unpack_from(">I", data, record)[0]
    if len(new) <= len(old):
        data[name_at:name_at + len(old) + 1] = new.encode("ascii").ljust(len(old) + 1, b"\0")
    else:
        # The name pointer is relocated already; it just points at the name's new home past the end.
        struct.pack_into(">I", data, record, len(data))
        data += new.encode("ascii") + b"\0"
    rows = [struct.unpack_from(">III", data, index + 12 * i) for i in range(count)]
    rows = [(elf_hash(new) if r == record else h, z, r) for h, z, r in rows]
    for i, row in enumerate(sorted(rows)):
        struct.pack_into(">III", data, index + 12 * i, *row)
    return bytes(data)


def copy_level(source: str, old: str, new: str) -> bytes:
    """The archive at `source` (the level `old`) as the level `new`: write it as level_file_name(new)."""
    archive = Caff.load(source)
    entry = database_entry(archive)
    return archive.write({entry.index: rename_level(archive, old, new)})
