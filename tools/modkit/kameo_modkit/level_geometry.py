"""Replaces a level's background geometry (its room or terrain, the rendergraph drawn through ndBGPushBuffer) with a new
mesh: one or more parts, each drawn with one of the room's own materials, so its shaders, textures and lightmap stay valid.

How the game draws a level's background:
  The rendergraph's command stream (its +132 header; the stream starts at the header's +0) is what draws, not the node
  tree. Per material it selects the pass (0x16), sets shaders, textures and the vertex buffer (0x2E, from the
  ndVertexBuffer2 stream table, whose D3D headers ndVertexBuffer2InitNode builds from each record's data and size), then
  one 0x31 packet per ndBGPushBuffer draw: [len 0x14, type 0x31][primitive type][index count][background index]
  [D3D index buffer header, the push buffer's +52 list]. ndPushBufferInitNode builds that header from the push buffer's
  +64 primitive type, +68 primitive count and +60 index data (16-bit, big-endian in memory, in .gpu).
  A 0x31 draw runs only when its background index's bit is set in the instance's background mask (sgIteratorSetup
  loads it into dword_8275D9E4). 0x17 packets skip what an ndVisibility node holds unless its level part
  (|_PART_..., levelSetPartVisible) is shown.
Visibility: every frame the level's culling pass (xfixgem_scenegraph_popStateStack) clears the background mask and
  octreeCullVisibility walks the rendergraph's octree (+76, a "visibility" asset of type 1): 44-byte nodes from its
  +52, each +0 flags (0x40000000 leaf) and instance offset, +4 a float AABB (min, max), an inner node's eight child
  numbers at +28 (u16, 0 none) and a leaf's background index list at +32 with its count at +36 (u16). A leaf whose AABB
  meets the view frustum sets its indices' bits. ndBGPushBuffer keeps its own AABB at +84 and its index at +108;
  ndShaderParam2WithVisibility the range of indices under it (+84 first, +88 count).
Lighting is baked: the room's materials sample a shared lightmap atlas ("ambient0"/"ambientshadowmap") through the
  second UV set, and the vertex layout carries more baked terms (ubyte4 and DEC3N texcoords) beside the normal frame.

What replacing does:
  Each part keeps one ndBGPushBuffer under its material (one outside any ndVisibility, whose index an octree leaf
  holds) and writes the part into it as a triangle list: index data, primitive type and count, vertex count, the 0x31
  packet's primitive type and index count, and the push buffer's AABB. The vertex buffer under it is written again
  with only the new vertices. Every other background draw of the graph is emptied: its primitive count becomes 0 and
  its 0x31 packet becomes a packet type the stream walker skips (0x00), so it draws nothing in any pass. The octree
  leaves holding a kept index, and every node above them, grow to hold the part, and the rendergraph's bounding
  sphere (+56: radius, centre) grows to hold the octree's root, so the new geometry is culled only when truly off
  screen.
  A new vertex starts as a copy of one of its material's old vertices, chosen by facing (the nearest normal, and among
  those the one on the most typical, evenly lit lightmap texels), so the baked terms the part does not give, the lightmap UV above all, come from
  a real lit surface: each face direction samples one lightmap texel and is lit evenly. Position, normal, tangent and
  binormal (along +U and -V, as the importer makes them), UVs and colour are written over that copy.
Collision is separate (hits.py).
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

import numpy as np

from .caff import Caff
from .graph import Node, Ref, Resolver, rendergraph_roots
from .importer import Edits, Gltf, _normalised, _point, _relayout, _tangents, _unflip, _vector, read_layout, DRAW_NAME
from .material import read_material, texture_palette
from .mesh import BINORMAL, COLOR, NORMAL, POSITION, TANGENT, TEXCOORD, Draw, Mesh, collect_meshes, decode_element, encode_element
from .texture import read_texture, to_pixels

PRIMITIVE_TRIANGLE_LIST = 4
PACKET_BG_DRAW = 0x31
PACKET_END = 0x1D
PACKET_SKIPPED = 0x00  # no case in __sgCommandStreamRender: the walker steps over it by its length
LEAF = 0x40000000
NODE_SIZE = 44


class GeometryError(Exception):
    pass


@dataclass
class Part:
    """A piece of the new background: its vertices in game space, triangles wound counter-clockwise seen from the side
    they face (cross(b - a, c - a) along the normal, as the room's own are), and the material slot it is drawn with
    (a number from background_materials, or the name of one of the slot's textures, such as "colourfile50")."""
    material: int | str
    positions: list[tuple[float, float, float]]
    triangles: list[tuple[int, int, int]]
    uvs: list[tuple[float, float]] | None = None  # TEXCOORD0; planar from the dominant axis when not given
    normals: list[tuple[float, float, float]] | None = None  # from the faces when not given
    colours: list[tuple[float, float, float, float]] | None = None  # RGBA, written only where the layout has a colour
    lightmap_uvs: list[tuple[float, float]] | None = None  # TEXCOORD1; the chosen old vertex's when not given


@dataclass
class Slot:
    """A material of the background, with the vertex buffer it draws from."""
    number: int
    mesh: int  # collect_meshes number in the graph
    material: Node
    flags: int  # the material node's render flags (+6): the room's opaque draws are 0x4701
    draws: list[Draw] = field(default_factory=list)
    textures: list[str] = field(default_factory=list)  # "pal<index> <w>x<h> <format> <name>"

    @property
    def triangles(self) -> int:
        return sum(len(d.triangles()) for d in self.draws)


@dataclass
class _Background:
    resolver: Resolver
    entry: object
    roots: list[Node]
    meshes: list[Mesh]
    slots: list[Slot]
    hidden: set[tuple[int, int]]  # push buffers under an ndVisibility (entry, offset)
    packets: dict[tuple[int, int], list[Ref]]  # index buffer header (entry, offset) -> its 0x31 packets
    octree: Ref | None


def _rendergraph(archive: Caff, resolver: Resolver, graph: int):
    graphs = [e for e in archive.entries if resolver.type_name(e) == "rendergraph"]
    if graph >= len(graphs):
        raise GeometryError(f"the archive has {len(graphs)} rendergraphs; there is no graph {graph}")
    return graphs[graph]


def _draw_packets(resolver: Resolver, entry) -> dict[tuple[int, int], list[Ref]]:
    header = resolver.pointer(Ref(entry, 0), 132)
    out: dict[tuple[int, int], list[Ref]] = {}
    if header is None:
        return out
    packet = resolver.pointer(header, 0)
    data = resolver.data(packet.entry)
    offset = packet.offset
    while offset + 4 <= len(data):
        length, kind = struct.unpack_from(">H", data, offset)[0], data[offset + 2]
        if kind == PACKET_END or length == 0:
            break
        if kind == PACKET_BG_DRAW:
            target = resolver.pointer(Ref(packet.entry, offset), 16)
            if target is not None:
                out.setdefault((target.entry.index, target.offset), []).append(Ref(packet.entry, offset))
        offset += length
    return out


def _read_background(archive: Caff, graph: int) -> _Background:
    resolver = Resolver(archive)
    entry = _rendergraph(archive, resolver, graph)
    roots = rendergraph_roots(resolver, entry)
    meshes = collect_meshes(resolver, roots)
    hidden: set[tuple[int, int]] = set()

    def mark(nodes: list[Node], inside: bool) -> None:
        for node in nodes:
            under = inside or node.type == "ndVisibility"
            if under and node.type == "ndBGPushBuffer":
                hidden.add((node.ref.entry.index, node.ref.offset))
            mark(node.children, under)

    mark(roots, False)
    palette = texture_palette(resolver, Ref(entry, 0))
    slots: list[Slot] = []
    by_key: dict[tuple, Slot] = {}
    for number, mesh in enumerate(meshes):
        for draw in mesh.draws:
            if draw.push is None or draw.push.type != "ndBGPushBuffer" or draw.material is None:
                continue
            key = (number, draw.material.ref.entry.index, draw.material.ref.offset)
            if key not in by_key:
                slot = Slot(len(slots), number, draw.material, resolver.u16(draw.material.ref, 6))
                for sampler in read_material(resolver, draw.material).samplers:
                    if sampler.dynamic or sampler.palette_index >= len(palette) or palette[sampler.palette_index] is None:
                        continue
                    texture = read_texture(resolver, palette[sampler.palette_index])
                    slot.textures.append(f"pal{sampler.palette_index} {texture.width}x{texture.height} {texture.format_name} {sampler.file_node}".rstrip())
                by_key[key] = slot
                slots.append(slot)
            by_key[key].draws.append(draw)
    octree = resolver.pointer(Ref(entry, 0), 76)
    return _Background(resolver, entry, roots, meshes, slots, hidden, _draw_packets(resolver, entry), octree)


def background_materials(path: str, graph: int = 0) -> list[Slot]:
    """The materials a level's background draws with, numbered for Part.material."""
    return _read_background(Caff.load(path), graph).slots


# --- the octree ----------------------------------------------------------------------------------------------------

@dataclass
class _OctreeNode:
    ref: Ref
    parent: int
    leaf: bool
    indices: list[int]


def _read_octree(resolver: Resolver, visibility: Ref | None) -> list[_OctreeNode]:
    """The octree's nodes by number, or none when the graph has no octree (a portal graph, or none at all)."""
    if visibility is None or resolver.u32(visibility, 28) != 1:
        return []
    base = resolver.pointer(visibility, 52)
    if base is None or resolver.u32(visibility, 44) != 1 or not resolver.u32(base, 0) & 0x80000000:
        raise GeometryError("the level's octree keeps packed bounds; only float bounds are understood")
    nodes: dict[int, _OctreeNode] = {}
    stack = [(0, -1)]
    while stack:
        number, parent = stack.pop()
        if number in nodes:
            continue
        ref = Ref(base.entry, base.offset + NODE_SIZE * number)
        leaf = bool(resolver.u32(ref, 0) & LEAF)
        indices = []
        if leaf:
            listed = resolver.pointer(ref, 32)
            indices = [resolver.u32(listed, 4 * i) for i in range(resolver.u16(ref, 36))] if listed else []
        else:
            stack.extend((resolver.u16(ref, 28 + 2 * k), number) for k in range(8) if resolver.u16(ref, 28 + 2 * k))
        nodes[number] = _OctreeNode(ref, parent, leaf, indices)
    return [nodes[n] for n in sorted(nodes)] if nodes else []


def _octree_by_number(resolver: Resolver, visibility: Ref | None) -> dict[int, _OctreeNode]:
    base = resolver.pointer(visibility, 52) if visibility is not None else None
    out = {}
    for node in _read_octree(resolver, visibility):
        out[(node.ref.offset - base.offset) // NODE_SIZE] = node
    return out


def _aabb(resolver: Resolver, ref: Ref, offset: int) -> tuple[list[float], list[float]]:
    values = [resolver.f32(ref, offset + 4 * k) for k in range(6)]
    return values[:3], values[3:]


# --- building ------------------------------------------------------------------------------------------------------

def _face_normals(positions, triangles) -> list[tuple[float, float, float]]:
    sums = [[0.0, 0.0, 0.0] for _ in positions]
    for a, b, c in triangles:
        e1 = [positions[b][k] - positions[a][k] for k in range(3)]
        e2 = [positions[c][k] - positions[a][k] for k in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        for v in (a, b, c):
            for k in range(3):
                sums[v][k] += n[k]
    return [_normalised(s) or (0.0, 1.0, 0.0) for s in sums]


def _planar_uvs(positions, normals, scale: float = 0.125) -> list[tuple[float, float]]:
    out = []
    for p, n in zip(positions, normals):
        axis = max(range(3), key=lambda k: abs(n[k]))
        if axis == 1:
            out.append((p[0] * scale, p[2] * scale))
        elif axis == 0:
            out.append((p[2] * scale, -p[1] * scale))
        else:
            out.append((p[0] * scale, -p[1] * scale))
    return out


class _TemplatePicker:
    """Chooses, for a facing, the old vertex of the slot's draws a new vertex is copied from."""

    def __init__(self, resolver: Resolver, entry, slot: Slot, mesh: Mesh):
        self.mesh = mesh
        used = sorted({v for d in slot.draws for t in d.triangles() for v in t if v < len(mesh.positions)})
        if not used:
            used = list(range(len(mesh.positions)))
        self.used = used
        self.normals = mesh.attributes.get((NORMAL, 0), [])
        self.light = self._lightmap_fit(resolver, entry, slot, mesh, used)
        self.cache: dict[tuple, int] = {}

    @staticmethod
    def _lightmap_fit(resolver: Resolver, entry, slot: Slot, mesh: Mesh, used: list[int]) -> dict[int, float]:
        """Per old vertex, how far the lightmap around its second UV is from a typical well lit spot of the material: the
        3x3 texels' distance from the 75th percentile of all its vertices' texels, channel by channel, plus their
        spread. Lower is better. The atlas's channels are separate terms (ambient, shadow and a rarer one), so the
        brightest texel is not the most ordinary lit one."""
        material = read_material(resolver, slot.material)
        palette = texture_palette(resolver, Ref(entry, 0))
        sampler = next((s for s in material.samplers if s.file_node.startswith("ambient") or any(name.startswith("ambient") and number == s.slot for name, number, _ in material.names)), None)
        uvs = mesh.attributes.get((TEXCOORD, 1))
        if sampler is None or uvs is None or sampler.palette_index >= len(palette) or palette[sampler.palette_index] is None:
            return {}
        pixels = to_pixels(resolver, read_texture(resolver, palette[sampler.palette_index]))[:, :, :3].astype(float)
        height, width = pixels.shape[:2]
        windows = {}
        for v in used:
            x, y = int((uvs[v][0] % 1.0) * width), int((uvs[v][1] % 1.0) * height)
            window = pixels[max(0, y - 1):y + 2, max(0, x - 1):x + 2].reshape(-1, 3)
            if window.size:
                windows[v] = window
        if not windows:
            return {}
        target = np.percentile(np.array([w[len(w) // 2] for w in windows.values()]), 75, axis=0)
        return {v: float(np.abs(w.mean(axis=0) - target).sum() + (w.max(axis=0) - w.min(axis=0)).sum()) for v, w in windows.items()}

    def pick(self, normal) -> int:
        key = tuple(round(c, 2) for c in normal)
        if key in self.cache:
            return self.cache[key]
        if not self.normals:
            self.cache[key] = self.used[0]
            return self.used[0]

        def facing(v):
            n = _normalised(self.normals[v]) or (0.0, 0.0, 0.0)
            return sum(n[k] * normal[k] for k in range(3))

        scored = sorted(self.used, key=facing, reverse=True)
        best = facing(scored[0])
        close = [v for v in scored if facing(v) >= best - 0.05] or scored[:1]
        choice = min(close, key=lambda v: (self.light.get(v, 0.0), -facing(v)))
        self.cache[key] = choice
        return choice


def _resolve_slot(background: _Background, material: int | str) -> Slot:
    if isinstance(material, int):
        if not 0 <= material < len(background.slots):
            raise GeometryError(f"material slot {material} does not exist; the background has {len(background.slots)}")
        return background.slots[material]
    matches = [s for s in background.slots if any(material in (t.split() + [""] * 4)[3:4] or t.split()[0] == material for t in s.textures)]
    if not matches:
        raise GeometryError(f"no background material shows a texture named '{material}'")
    return matches[0]


def _choose_push(background: _Background, slot: Slot, taken: set, leaves_of: dict[int, list[int]]) -> Draw:
    resolver = background.resolver
    candidates = []
    for draw in slot.draws:
        key = (draw.push.ref.entry.index, draw.push.ref.offset)
        if key in taken or key in background.hidden:
            continue
        index = resolver.u32(draw.push.ref, 108)
        header = resolver.pointer(resolver.pointer(draw.push.ref, 52), 4 * draw.slot)
        if header is None or not background.packets.get((header.entry.index, header.offset)):
            continue
        if leaves_of and index not in leaves_of:
            continue
        candidates.append(draw)
    if not candidates:
        raise GeometryError(f"material slot {slot.number} has no draw outside the level's switchable parts that the octree and command stream both know")
    return max(candidates, key=lambda d: len(d.triangles()))


def replace_background(original: str, out: str, parts: list[Part], graph: int = 0) -> list[str]:
    """Writes `original` to `out` with graph `graph`'s background replaced by `parts`; returns notes on what was done."""
    archive = Caff.load(original)
    background = _read_background(archive, graph)
    resolver = background.resolver
    notes: list[str] = []
    if not background.slots:
        raise GeometryError(f"graph {graph} has no background (ndBGPushBuffer) draws")
    octree = _octree_by_number(resolver, background.octree)
    leaves_of: dict[int, list[int]] = {}
    for number, node in octree.items():
        for index in node.indices:
            leaves_of.setdefault(index, []).append(number)

    # Parts by slot; each slot keeps one push buffer.
    grouped: dict[int, list[Part]] = {}
    for part in parts:
        if len(part.positions) == 0 or len(part.triangles) == 0:
            continue
        for t in part.triangles:
            if max(t) >= len(part.positions) or min(t) < 0:
                raise GeometryError(f"a triangle {t} names a vertex beyond the part's {len(part.positions)}")
        grouped.setdefault(_resolve_slot(background, part.material).number, []).append(part)
    if not grouped:
        raise GeometryError("no part has any triangles")
    taken: set = set()
    kept: dict[int, Draw] = {}
    for number in grouped:
        slot = background.slots[number]
        if slot.flags != 0x4701:
            notes.append(f"slot {number}: its material's render flags are {slot.flags:#x}, not the room's opaque 0x4701; it may draw in another pass")
        draw = _choose_push(background, slot, taken, leaves_of)
        taken.add((draw.push.ref.entry.index, draw.push.ref.offset))
        kept[number] = draw

    edits = Edits(resolver)
    new_blocks: dict[tuple[int, int], bytes] = {}
    kept_draws = {id(d) for d in kept.values()}
    bounds_of: dict[int, tuple[list[float], list[float]]] = {}

    # Vertex buffers: every slot's parts on one mesh share it.
    by_mesh: dict[int, list[int]] = {}
    for number in grouped:
        by_mesh.setdefault(background.slots[number].mesh, []).append(number)
    for mesh_number, slot_numbers in by_mesh.items():
        mesh = background.meshes[mesh_number]
        streams, _ = read_layout(resolver, mesh.node)
        old_count = len(mesh.positions)
        for stream in streams:
            if stream.data and stream.stride and stream.size // stream.stride != old_count:
                raise GeometryError(f"mesh {mesh_number}: a vertex stream of {stream.size // stream.stride} vertices beside {old_count}; this layout is not understood")
        old_bytes = [resolver.bytes(s.data, s.size) if s.data else b"" for s in streams]
        rows = [bytearray() for _ in streams]
        count = 0
        for number in slot_numbers:
            slot = background.slots[number]
            picker = _TemplatePicker(resolver, background.entry, slot, mesh)
            indices: list[int] = []
            lo, hi = [math.inf] * 3, [-math.inf] * 3
            for part in grouped[number]:
                positions = [tuple(float(c) for c in p[:3]) for p in part.positions]
                normals = [(_normalised(n) or (0.0, 1.0, 0.0)) for n in part.normals] if part.normals else _face_normals(positions, part.triangles)
                uvs = [tuple(uv[:2]) for uv in part.uvs] if part.uvs else _planar_uvs(positions, normals)
                tangents, binormals = _tangents(positions, uvs, normals, part.triangles)
                for v, p in enumerate(positions):
                    for k in range(3):
                        lo[k], hi[k] = min(lo[k], p[k]), max(hi[k], p[k])
                    old = picker.pick(normals[v])
                    for s, stream in enumerate(streams):
                        if not stream.data:
                            continue
                        row = bytearray(old_bytes[s][old * stream.stride:(old + 1) * stream.stride])
                        for element in stream.elements:
                            values = None
                            if element.usage == POSITION and element.usage_index == 0:
                                values = tuple(c / mesh.position_scale for c in p)
                                if element.type == 0x1A235A:
                                    values += (struct.unpack_from(">4h", row, element.offset)[3],)
                            elif element.usage == NORMAL and element.usage_index == 0:
                                values = normals[v]
                            elif element.usage == TANGENT and element.usage_index == 0 and tangents[v]:
                                values = tangents[v]
                            elif element.usage == BINORMAL and element.usage_index == 0 and binormals[v]:
                                values = binormals[v]
                            elif element.usage == TEXCOORD and element.usage_index == 0:
                                values = (uvs[v][0] / mesh.uv_scale, uvs[v][1] / mesh.uv_scale)
                            elif element.usage == TEXCOORD and element.usage_index == 1 and part.lightmap_uvs:
                                values = (part.lightmap_uvs[v][0] / mesh.uv_scale, part.lightmap_uvs[v][1] / mesh.uv_scale)
                            elif element.usage == COLOR and element.usage_index == 0 and part.colours:
                                r, g, b, a = (tuple(part.colours[v]) + (1.0,))[:4]
                                values = (a, r, g, b)
                            if values is None:
                                continue
                            size = _element_size(element.type)
                            base = bytes(row[element.offset:element.offset + size])
                            if element.type in (0x2C23A5, 0x2A23B9, 0x1A23A6, 0x2C83A4):
                                width = {0x2C83A4: 1, 0x2C23A5: 2, 0x2A23B9: 3, 0x1A23A6: 4}[element.type]
                                values = tuple(values[:width]) + tuple(decode_element(base, 0, element.type)[len(values):width])
                            row[element.offset:element.offset + size] = encode_element(element.type, tuple(values), base)
                        rows[s] += row
                indices.extend(count + i for t in part.triangles for i in t)
                count += len(positions)
            if count > 0xFFFF:
                raise GeometryError(f"mesh {mesh_number}: {count} vertices, more than 16-bit indices reach (65535)")
            bounds_of[number] = (lo, hi)
            _write_draw(resolver, edits, new_blocks, background, kept[number], indices, lo, hi)
        for s, stream in enumerate(streams):
            if stream.data:
                edits.u32(stream.record, 16, len(rows[s]))
                new_blocks[(stream.record.entry.index, stream.record.offset + 12)] = bytes(rows[s])
        notes.append(f"mesh {mesh_number}: vertex buffer rewritten with {count} vertices ({old_count} before)")

    # Every other background draw of the graph draws nothing.
    emptied = 0
    for slot in background.slots:
        for draw in slot.draws:
            if id(draw) in kept_draws:
                continue
            edits.u32(resolver.pointer(draw.push.ref, 68), 4 * draw.slot, 0)
            header = resolver.pointer(resolver.pointer(draw.push.ref, 52), 4 * draw.slot)
            for packet in background.packets.get((header.entry.index, header.offset), []) if header else []:
                edits.u8(packet, 2, PACKET_SKIPPED)
            emptied += 1
    notes.append(f"{emptied} other background draws emptied")

    # Culling: the leaves holding a kept index, and the nodes above them, grow to hold its part.
    for number, draw in kept.items():
        index = resolver.u32(draw.push.ref, 108)
        lo, hi = bounds_of[number]
        grown = set()
        for leaf in leaves_of.get(index, []):
            node = leaf
            while node >= 0 and node not in grown:
                grown.add(node)
                ref = octree[node].ref
                old_lo, old_hi = _aabb(resolver, ref, 4)
                for k in range(3):
                    edits.f32(ref, 4 + 4 * k, min(old_lo[k], lo[k]))
                    edits.f32(ref, 16 + 4 * k, max(old_hi[k], hi[k]))
                node = octree[node].parent
        notes.append(f"slot {number}: drawn by the push buffer at {draw.push.ref.offset:#x} (background index {index}), {len(leaves_of.get(index, []))} octree leaves and {len(grown)} nodes grown")
    if octree:
        root = octree[0].ref
        values = [struct.unpack_from(">f", edits.entry(root.entry.index), root.offset + 4 + 4 * k)[0] for k in range(6)]
        centre = [(values[k] + values[3 + k]) / 2 for k in range(3)]
        radius = math.sqrt(sum(((values[3 + k] - values[k]) / 2) ** 2 for k in range(3)))
        asset = Ref(background.entry, 0)
        old_radius = resolver.f32(asset, 56)
        old_centre = [resolver.f32(asset, 60 + 4 * k) for k in range(3)]
        # The new sphere holds the old one too.
        radius = max(radius, math.dist(centre, old_centre) + old_radius)
        edits.f32(asset, 56, radius)
        for k in range(3):
            edits.f32(asset, 60 + 4 * k, centre[k])
    else:
        notes.append("the graph has no octree; nothing culls its background by bounds")

    replaced = _relayout(resolver, edits, new_blocks)
    with open(out, "wb") as handle:
        handle.write(archive.write(replaced=replaced))
    return notes


def _element_size(type_code: int) -> int:
    from .mesh import DECL_TYPES

    return DECL_TYPES[type_code][3]


def _write_draw(resolver: Resolver, edits: Edits, new_blocks, background: _Background, draw: Draw, indices: list[int], lo, hi) -> None:
    push = draw.push.ref
    slot = draw.slot
    edits.u32(resolver.pointer(push, 56), 4 * slot, max(indices) + 1)
    edits.u32(resolver.pointer(push, 64), 4 * slot, PRIMITIVE_TRIANGLE_LIST)
    edits.u32(resolver.pointer(push, 68), 4 * slot, len(indices) // 3)
    array = resolver.pointer(push, 60)
    new_blocks[(array.entry.index, array.offset + 4 * slot)] = struct.pack(f">{len(indices)}H", *indices)
    for k in range(3):
        edits.f32(push, 84 + 4 * k, lo[k])
        edits.f32(push, 96 + 4 * k, hi[k])
    header = resolver.pointer(resolver.pointer(push, 52), 4 * slot)
    for packet in background.packets[(header.entry.index, header.offset)]:
        edits.u32(packet, 4, PRIMITIVE_TRIANGLE_LIST)
        edits.u32(packet, 8, len(indices))


# --- inputs ----------------------------------------------------------------------------------------------------------

def arena_parts(floor_material: int | str, wall_material: int | str, half: float = 60.0, height: float = 12.0, floor_y: float = 0.0, floor_tile: float = 8.0, wall_tile: float = 6.0) -> list[Part]:
    """A square floor 2*half across centred on the origin at floor_y and four walls `height` high around it, facing
    inward; UVs repeat every floor_tile / wall_tile units."""
    y0, y1 = floor_y, floor_y + height
    floor = Part(floor_material,
                 [(-half, y0, -half), (half, y0, -half), (half, y0, half), (-half, y0, half)],
                 [(0, 2, 1), (0, 3, 2)],
                 normals=[(0.0, 1.0, 0.0)] * 4)
    floor.uvs = [(p[0] / floor_tile, p[2] / floor_tile) for p in floor.positions]
    positions, triangles, normals, uvs = [], [], [], []
    corners = [(-half, -half), (half, -half), (half, half), (-half, half)]
    for i in range(4):
        (x0, z0), (x1, z1) = corners[i], corners[(i + 1) % 4]
        inward = _normalised((-(x0 + x1) / 2, 0.0, -(z0 + z1) / 2))
        base = len(positions)
        positions += [(x0, y0, z0), (x1, y0, z1), (x1, y1, z1), (x0, y1, z0)]
        length = math.dist((x0, z0), (x1, z1))
        uvs += [(0.0, 0.0), (length / wall_tile, 0.0), (length / wall_tile, -height / wall_tile), (0.0, -height / wall_tile)]
        normals += [inward] * 4
        a, b, c, d = base, base + 1, base + 2, base + 3
        # Wound so cross(b - a, c - a) points inward.
        e1 = [positions[b][k] - positions[a][k] for k in range(3)]
        e2 = [positions[c][k] - positions[a][k] for k in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if sum(n[k] * inward[k] for k in range(3)) > 0:
            triangles += [(a, b, c), (a, c, d)]
        else:
            triangles += [(a, c, b), (a, d, c)]
    walls = Part(wall_material, positions, triangles, uvs=uvs, normals=normals)
    return [floor, walls]


def parts_from_gltf(path: str, original: str, graph: int = 0) -> list[Part]:
    """Parts from a glTF whose materials are named as the level's export names its draws (graph<g>_mesh<m>_draw<d>):
    each triangle is drawn with that draw's material; objects of other graphs (a whole level export's props) are left out.
    Positions come back from the export's mirrored X."""
    background = _read_background(Caff.load(original), graph)
    slot_of: dict[tuple[int, int], int] = {}
    for slot in background.slots:
        mesh = background.meshes[slot.mesh]
        for draw in slot.draws:
            slot_of[(slot.mesh, mesh.draws.index(draw))] = slot.number
    gltf = Gltf(path)
    materials = gltf.doc.get("materials", [])
    parts: dict[int, Part] = {}
    for node_index, node in enumerate(gltf.doc.get("nodes", [])):
        if "mesh" not in node:
            continue
        world = gltf.world[node_index]
        for primitive in gltf.doc["meshes"][node["mesh"]]["primitives"]:
            name = materials[primitive["material"]].get("name", "") if "material" in primitive else ""
            match = DRAW_NAME.search(name)
            if match and int(match.group(1)) != graph:
                continue  # another graph's object (a prop), as a whole level's export holds; this replaces only the background
            if not match or (int(match.group(2)), int(match.group(3))) not in slot_of:
                raise GeometryError(f"{node.get('name', node_index)}: material '{name or 'none'}' names no background draw of graph {graph}")
            number = slot_of[(int(match.group(2)), int(match.group(3)))]
            part = parts.setdefault(number, Part(number, [], [], uvs=[], normals=[], colours=None))
            attributes = primitive["attributes"]
            positions = gltf.accessor(attributes["POSITION"])
            normals = gltf.accessor(attributes["NORMAL"]) if "NORMAL" in attributes else None
            uvs = gltf.accessor(attributes["TEXCOORD_0"]) if "TEXCOORD_0" in attributes else None
            base = len(part.positions)
            for i, p in enumerate(positions):
                part.positions.append(_unflip(_point(world, p)))
                n = _normalised(_vector(world, normals[i])) if normals else None
                part.normals.append(_unflip(n) if n else (0.0, 1.0, 0.0))
                part.uvs.append((uvs[i][0], uvs[i][1]) if uvs else (0.0, 0.0))
            indices = [int(i[0]) for i in gltf.accessor(primitive["indices"])] if "indices" in primitive else list(range(len(positions)))
            # The export reversed the winding with the mirrored X; reverse it back.
            part.triangles += [(base + indices[t], base + indices[t + 2], base + indices[t + 1]) for t in range(0, len(indices) - 2, 3)]
    return list(parts.values())
