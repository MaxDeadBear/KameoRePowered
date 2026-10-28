"""Decodes rendergraph geometry: ndVertexBuffer2 streams and declarations, and the ndPushBuffer draws under them.

ndVertexBuffer2 (ndVertexBuffer2InitNode, ndVertexBuffer2Render, ndVertexBuffer2SetVertexShader):
  +48 stream records, 20 bytes each: stride, ?, D3DVertexBuffer header, data (in .gpu), data size
  +52 variant set: count, variant pointers; a variant holds its stream indices (+0) and count (+8),
      c_posscale register (+18) and value (+52), c_uvscale register (+20) and value (+56), the
      D3DVERTEXELEMENT9 declaration (+28), vertex shader (+32) and texture samplers (+68, count +72)
  +56 stream count
ndPushBuffer (ndPushBufferRender): +48 draw count, +52 index buffer headers, +56 vertex counts,
  +60 index data (16-bit, in .gpu), +64 primitive types, +68 primitive counts; indices are absolute
  within the vertex buffer the draw sits under.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .graph import Node, Ref, Resolver

POSITION, BLENDWEIGHT, BLENDINDICES, NORMAL, PSIZE, TEXCOORD, TANGENT, BINORMAL = 0, 1, 2, 3, 4, 5, 6, 7
COLOR = 10

# XGVERTEXFORMAT values: (struct format, component count, normalise divisor or None, size).
DECL_TYPES = {
    0x2C83A4: (">f", 1, None, 4), 0x2C23A5: (">2f", 2, None, 8), 0x2A23B9: (">3f", 3, None, 12), 0x1A23A6: (">4f", 4, None, 16),
    0x182886: ("BGRA", 4, 255.0, 4), 0x1A2286: (">4B", 4, None, 4), 0x1A2386: (">4B", 4, None, 4),
    0x2C2359: (">2h", 2, None, 4), 0x1A235A: (">4h", 4, None, 8), 0x1A2086: (">4B", 4, 255.0, 4), 0x1A2186: (">4B", 4, 255.0, 4),
    0x2C2159: (">2h", 2, 32767.0, 4), 0x1A215A: (">4h", 4, 32767.0, 8), 0x2C2059: (">2H", 2, 65535.0, 4), 0x1A205A: (">4H", 4, 65535.0, 8),
    0x2C82A1: (">I", 1, None, 4), 0x2A2287: ("UDEC3", 3, None, 4), 0x2A2187: ("DEC3N", 3, None, 4), 0x2A2190: ("DEC3N", 3, None, 4),
    0x2A2390: ("DEC3N", 3, None, 4), 0x2C235F: ("HALF2", 2, None, 4), 0x1A2360: ("HALF4", 4, None, 8),
}
PRIMITIVE_TRIANGLE_LIST, PRIMITIVE_TRIANGLE_STRIP = 4, 6


@dataclass
class Element:
    stream: int
    offset: int
    type: int
    usage: int
    usage_index: int


@dataclass
class Draw:
    primitive: int
    primitive_count: int
    vertex_count: int
    indices: list[int]
    material: Node | None = None
    # The draw's bone palette from the ndRigidSkinIdx above it: matrix indices into the skeleton's ndMtxArray. A vertex's
    # BLENDINDICES are vertex shader register offsets, three registers per matrix, so index // 3 is the slot in this list.
    palette: list[int] = field(default_factory=list)
    skin: Node | None = None  # that ndRigidSkinIdx
    push: Node | None = None  # the ndPushBuffer holding the draw, and its slot there
    slot: int = 0
    palette_ref: Ref | None = None  # an added draw's own palette list, where the importer gave it one

    def triangles(self) -> list[tuple[int, int, int]]:
        out = []
        if self.primitive == PRIMITIVE_TRIANGLE_LIST:
            for i in range(0, len(self.indices) - 2, 3):
                out.append((self.indices[i], self.indices[i + 1], self.indices[i + 2]))
        elif self.primitive == PRIMITIVE_TRIANGLE_STRIP:
            for i in range(len(self.indices) - 2):
                a, b, c = self.indices[i], self.indices[i + 1], self.indices[i + 2]
                if a == b or b == c or a == c:
                    continue
                # An odd triangle is wound the other way; its first corner, the provoking vertex, stays first.
                out.append((a, b, c) if i % 2 == 0 else (a, c, b))
        return out


@dataclass
class Mesh:
    node: Node
    elements: list[Element]
    position_scale: float
    uv_scale: float
    attributes: dict[tuple[int, int], list[tuple[float, ...]]] = field(default_factory=dict)
    draws: list[Draw] = field(default_factory=list)
    samplers: list[int] = field(default_factory=list)

    _scaled: dict = field(default_factory=dict, repr=False)

    @property
    def positions(self) -> list[tuple[float, float, float]]:
        if "positions" not in self._scaled:
            self._scaled["positions"] = [(x * self.position_scale, y * self.position_scale, z * self.position_scale) for x, y, z, *_ in self.attributes.get((POSITION, 0), [])]
        return self._scaled["positions"]

    def uvs(self, index: int = 0) -> list[tuple[float, float]]:
        if index not in self._scaled:
            self._scaled[index] = [(u * self.uv_scale, v * self.uv_scale) for u, v, *_ in self.attributes.get((TEXCOORD, index), [])]
        return self._scaled[index]


def _half(value: int) -> float:
    sign = -1.0 if value & 0x8000 else 1.0
    exponent = (value >> 10) & 0x1F
    fraction = value & 0x3FF
    if exponent == 0:
        return sign * fraction * 2.0 ** -20
    if exponent == 31:
        return sign * float("inf")
    return sign * (1.0 + fraction / 1024.0) * 2.0 ** (exponent - 15)


def _dec3n(value: int, signed: bool) -> tuple[float, float, float]:
    out = []
    for shift in (0, 10, 20):
        part = (value >> shift) & 0x3FF
        if signed:
            if part & 0x200:
                part -= 0x400
            out.append(max(part / 511.0, -1.0))
        else:
            out.append(part / 1023.0)
    return tuple(out)


def decode_element(data: bytes, offset: int, type_code: int) -> tuple[float, ...]:
    fmt, count, divisor, _ = DECL_TYPES[type_code]
    if fmt == "DEC3N" or fmt == "UDEC3":
        return _dec3n(struct.unpack_from(">I", data, offset)[0], fmt == "DEC3N")
    if fmt == "HALF2" or fmt == "HALF4":
        return tuple(_half(v) for v in struct.unpack_from(">%dH" % count, data, offset))
    if fmt == "BGRA":
        b, g, r, a = struct.unpack_from(">4B", data, offset)
        return (r / 255.0, g / 255.0, b / 255.0, a / 255.0)
    values = struct.unpack_from(fmt, data, offset)
    return tuple(v / divisor for v in values) if divisor else tuple(float(v) for v in values)


def encode_element(type_code: int, values: tuple[float, ...], base: bytes) -> bytes:
    """`values` packed as decode_element would read them back; `base`, the element's old bytes, keeps a DEC3N's two spare bits."""
    fmt, count, divisor, size = DECL_TYPES[type_code]
    if fmt == "DEC3N":
        packed = struct.unpack(">I", base)[0] & 0xC0000000
        for shift, value in zip((0, 10, 20), values):
            packed |= (max(-511, min(511, round(value * 511.0))) & 0x3FF) << shift
        return struct.pack(">I", packed)
    if fmt in ("UDEC3", "HALF2", "HALF4", "BGRA"):
        raise ValueError(f"writing vertex format {type_code:#x} is not supported")
    code = fmt[-1]
    limits = {"b": (-128, 127), "B": (0, 255), "h": (-32768, 32767), "H": (0, 65535), "I": (0, 0xFFFFFFFF)}.get(code)
    out = []
    for value in values[:count]:
        if divisor:
            value *= divisor
        if limits:
            value = max(limits[0], min(limits[1], round(value)))
        out.append(value)
    return struct.pack(fmt, *out)


def read_declaration(resolver: Resolver, ref: Ref) -> list[Element]:
    elements = []
    for i in range(64):
        item = Ref(ref.entry, ref.offset + 12 * i)
        stream = resolver.u16(item, 0)
        if stream == 0xFF or stream == 0xFFFF:
            break
        elements.append(Element(stream, resolver.u16(item, 2), resolver.u32(item, 4), resolver.u8(item, 9), resolver.u8(item, 10)))
    return elements


def read_vertex_buffer(resolver: Resolver, node: Node) -> Mesh:
    variants = resolver.pointer(node.ref, 52)
    variant = resolver.pointer(resolver.pointer(variants, 4), 0)
    elements = read_declaration(resolver, resolver.pointer(variant, 28))
    mesh = Mesh(node, elements, resolver.f32(variant, 52) if resolver.u16(variant, 18) != 0xFFFF else 1.0, resolver.f32(variant, 56) if resolver.u16(variant, 20) != 0xFFFF else 1.0)
    samplers = resolver.pointer(variant, 68)
    for i in range(resolver.u32(variant, 72) if samplers else 0):
        mesh.samplers.append(resolver.u32(samplers, 36 * i))
    stream_indices = resolver.pointer(variant, 0)
    stream_table = resolver.pointer(node.ref, 48)
    for element in elements:
        record_index = resolver.u8(stream_indices, element.stream) if stream_indices else element.stream
        record = Ref(stream_table.entry, stream_table.offset + 20 * record_index)
        stride = resolver.u32(record, 0)
        data_ref = resolver.pointer(record, 12)
        size = resolver.u32(record, 16)
        data = resolver.bytes(data_ref, size)
        count = size // stride if stride else 0
        mesh.attributes[(element.usage, element.usage_index)] = [decode_element(data, i * stride + element.offset, element.type) for i in range(count)]
    return mesh


def read_push_buffer(resolver: Resolver, node: Node) -> list[Draw]:
    draws = []
    ref = node.ref
    for i in range(resolver.u32(ref, 48)):
        primitive = resolver.u32(resolver.pointer(ref, 64), 4 * i)
        primitive_count = resolver.u32(resolver.pointer(ref, 68), 4 * i)
        vertex_count = resolver.u32(resolver.pointer(ref, 56), 4 * i)
        index_data = resolver.pointer(resolver.pointer(ref, 60), 4 * i)
        index_count = primitive_count * 3 if primitive == PRIMITIVE_TRIANGLE_LIST else primitive_count + 2
        indices = list(struct.unpack_from(">%dH" % index_count, resolver.data(index_data.entry), index_data.offset))
        draws.append(Draw(primitive, primitive_count, vertex_count, indices))
    return draws


def has_second_uvs(mesh: Mesh) -> bool:
    """Whether TEXCOORD1 is a second UV set; on a blend-shape mesh it is the first shape's deltas, four shorts."""
    return any(e.usage == TEXCOORD and e.usage_index == 1 and e.type == 0x2C2359 for e in mesh.elements)


def read_palette(resolver: Resolver, skin: Node) -> list[int]:
    """ndRigidSkinIdxLoadMatrices: +48 the byte list of matrix indices, +52 its length."""
    count = resolver.u32(skin.ref, 52)
    first = resolver.pointer(skin.ref, 48)
    return [resolver.u8(first, i) for i in range(count)] if first else []


def collect_meshes(resolver: Resolver, roots: list[Node]) -> list[Mesh]:
    meshes: list[Mesh] = []

    def visit(nodes: list[Node], current: Mesh | None, material: Node | None, skin: Node | None) -> None:
        for node in nodes:
            owner = current
            owner_material = material
            owner_skin = skin
            if node.type in ("ndVertexBuffer2", "ndBlendShape2"):
                owner = read_vertex_buffer(resolver, node)
                meshes.append(owner)
            elif node.type in ("ndShaderParam2", "ndShaderParam2WithVisibility"):
                owner_material = node
            elif node.type == "ndRigidSkinIdx":
                owner_skin = node
            elif node.type == "ndPushBuffer" and current is not None:
                palette = read_palette(resolver, skin) if skin is not None else []
                for slot, draw in enumerate(read_push_buffer(resolver, node)):
                    draw.material = material
                    draw.palette = palette
                    draw.skin = skin
                    draw.push = node
                    draw.slot = slot
                    current.draws.append(draw)
            visit(node.children, owner, owner_material, owner_skin)

    visit(roots, None, None, None)
    return meshes
