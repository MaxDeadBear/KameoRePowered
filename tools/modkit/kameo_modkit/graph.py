"""Walks a CAFF archive's relocated pointers and its rendergraph node trees.

A rendergraph asset holds its root node list at +36 (count at +40).  Every node starts with its type name
pointer (+0), type index (+4), flags (+8), child (+20) and next sibling (+24); the fields after +48 depend on
the type (sgInit registers the types, InitGraph binds them by name).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .caff import Caff, Entry


@dataclass(frozen=True)
class Ref:
    entry: Entry
    offset: int


class Resolver:
    def __init__(self, archive: Caff):
        self.archive = archive
        self._pointers: dict[int, dict[int, int]] = {}
        for relocation in archive.relocations:
            table = self._pointers.setdefault(relocation.source, {})
            for offset in relocation.offsets:
                table[offset] = relocation.target
        self._data: dict[int, bytes] = {}

    def data(self, entry: Entry) -> bytes:
        if entry.index not in self._data:
            self._data[entry.index] = entry.read(self.archive)
        return self._data[entry.index]

    def u8(self, ref: Ref, offset: int = 0) -> int:
        return self.data(ref.entry)[ref.offset + offset]

    def u16(self, ref: Ref, offset: int = 0) -> int:
        return struct.unpack_from(">H", self.data(ref.entry), ref.offset + offset)[0]

    def u32(self, ref: Ref, offset: int = 0) -> int:
        return struct.unpack_from(">I", self.data(ref.entry), ref.offset + offset)[0]

    def f32(self, ref: Ref, offset: int = 0) -> float:
        return struct.unpack_from(">f", self.data(ref.entry), ref.offset + offset)[0]

    def bytes(self, ref: Ref, size: int, offset: int = 0) -> bytes:
        start = ref.offset + offset
        return self.data(ref.entry)[start:start + size]

    def pointer(self, ref: Ref, offset: int = 0) -> Ref | None:
        """The pointer field at `ref + offset`, or None when the loader would not relocate it."""
        field = ref.offset + offset
        target = self._pointers.get(ref.entry.index, {}).get(field)
        if target is None:
            return None
        return Ref(self.archive.entries[target - 1], self.u32(ref, offset))

    def string(self, ref: Ref | None) -> str:
        if ref is None:
            return ""
        data = self.data(ref.entry)
        end = data.find(b"\0", ref.offset)
        return data[ref.offset:end if end >= 0 else len(data)].decode("latin1")

    def type_name(self, entry: Entry) -> str:
        head = self.data(entry)[:32].split(b"\0")[0]
        return head.decode("ascii") if head and all(32 <= b < 127 for b in head) else ""


@dataclass
class Node:
    ref: Ref
    type: str
    children: list["Node"]


def walk(resolver: Resolver, first: Ref | None, depth: int = 0) -> list[Node]:
    nodes = []
    seen = set()
    while first is not None and (first.entry.index, first.offset) not in seen:
        seen.add((first.entry.index, first.offset))
        kind = resolver.string(resolver.pointer(first, 0))
        children = walk(resolver, resolver.pointer(first, 20), depth + 1) if depth < 64 else []
        nodes.append(Node(first, kind, children))
        first = resolver.pointer(first, 24)
    return nodes


def rendergraph_roots(resolver: Resolver, entry: Entry) -> list[Node]:
    asset = Ref(entry, 0)
    roots = resolver.pointer(asset, 36)
    count = resolver.u32(asset, 40)
    nodes = []
    for i in range(count if roots else 0):
        nodes.extend(walk(resolver, resolver.pointer(roots, 4 * i)))
    return nodes
