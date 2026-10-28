"""Reads the command streams a model is drawn from (the .stream section, __sgCommandStreamRender).

A stream is a run of packets: u16 size in bytes, u8 command, u8 flags, then the command's fields. The renderer steps
through them by size; some jump (0x16 by the shader variant's slot in a table, 0x18/0x27 conditionally, 0x29 by the
pass) and some return (0x1A-0x1D). Pointers inside packets are relocated like any other field, to the rendergraph
(index buffer headers, palettes, shaders) or to the stream itself (jump targets).

Commands seen in models: 0x01 draw (+4 primitive, +12 index count, +16 index buffer header), 0x02 pixel shader,
0x03-0x08 shader constants, 0x09 declaration and vertex shader, 0x0A/0x2E vertex streams (0x2E with the declaration and
shader), 0x0B/0x2F blend-shape streams, 0x0C/0x0D render state, 0x0E/0x0F sampler state, 0x10/0x2C/0x2D textures
(0x2C: +4 the texture palette index, +12 the sampler), 0x11/0x12 matrices, 0x13 bone palette, 0x14 global state,
0x16 variant jump, 0x1A-0x1D return, 0x23/0x24 push and pop state, 0x30/0x31 draw if a part is visible (+4 primitive,
+8 index count, +12 the visibility bit, +16 index buffer header).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .caff import Caff, Entry

NAMES = {
    0x01: "draw", 0x02: "pixel shader", 0x03: "vs const", 0x04: "vs const", 0x05: "vs const", 0x06: "ps const", 0x07: "ps const", 0x08: "ps const",
    0x09: "decl+vs", 0x0A: "streams", 0x0B: "blend streams", 0x0C: "render state", 0x0D: "render state", 0x0E: "sampler state", 0x0F: "sampler block",
    0x10: "texture (indirect)", 0x11: "matrices", 0x12: "matrix array", 0x13: "bone palette", 0x14: "global state", 0x15: "skip unless",
    0x16: "variant jump", 0x17: "branch", 0x18: "branch", 0x19: "jump", 0x1A: "return unless", 0x1B: "return unless", 0x1C: "return unless", 0x1D: "return",
    0x1F: "prefetch", 0x20: "callback", 0x21: "transform", 0x22: "2d matrices", 0x23: "push state", 0x24: "pop state", 0x25: "restore transform", 0x26: "pop matrices",
    0x27: "branch", 0x28: "return unless", 0x29: "pass jump", 0x2A: "no pixel shader", 0x2B: "draw tessellated", 0x2C: "texture", 0x2D: "all textures",
    0x2E: "streams+decl+vs", 0x2F: "blend streams+vs", 0x30: "draw if visible", 0x31: "draw if visible",
}


@dataclass
class Packet:
    offset: int
    size: int
    command: int
    flags: int
    pointers: dict[int, tuple[int, int]]  # field offset within the packet -> (target entry index, value)

    @property
    def name(self) -> str:
        return NAMES.get(self.command, f"0x{self.command:02x}")


def packets(archive: Caff, entry: Entry, start: int, end: int | None = None) -> list[Packet]:
    """The packets from `start` laid end to end, until a return, a size that does not fit, or `end`."""
    data = entry.read(archive)
    pointers = archive.pointers_from(entry)
    out = []
    offset = start
    end = len(data) if end is None else end
    while offset + 4 <= end:
        size, command, flags = struct.unpack_from(">HBB", data, offset)
        if size < 4 or offset + size > len(data):
            break
        out.append(Packet(offset, size, command, flags, {o - offset: (t.index, v) for o, (t, v) in pointers.items() if offset <= o < offset + size}))
        offset += size
        if command == 0x1D:
            break
    return out
