"""Imports a glTF (the export's, edited) back into the model archive it came from, rebuilding the meshes' vertex and
index buffers while the archive's skeleton, materials, shaders and node graph stay as they are.

How the glTF maps onto the archive:
  Triangles go to the draw their material names, graph<g>_mesh<m>_draw<d> (a Blender suffix such as ".001" is
  ignored), whatever object holds them, so objects can be joined, split or replaced freely. A draw no triangle names
  is hidden; with keep_missing, a mesh none of whose draws is named keeps its old geometry.
  Bones are the game's own, named joint_<index>. Vertices are taken to the game's model space through their skin
  (or their node, when not skinned) and X is negated back (see gltf.py).

How a vertex is written: its vertex buffer's layout and shaders are fixed, so the new vertex starts as a copy of the
nearest old vertex of the same draw, every stream of it, and the attributes the glTF gives are written over that copy:
position, normal, tangent and binormal (worked out again from the UVs: tangent along +U, binormal along -V), UVs,
colour and skin. Whatever else the layout holds, blend-shape deltas, a second UV set, comes from that nearest vertex.

Bone palettes (ndRigidSkinIdx) are rewritten in place: a palette keeps its length, the vertex shader's register
budget, so when the bones its draws use do not fit, the heaviest are kept and the others fall back to their nearest
kept ancestor (reported). A vertex without skin data takes its old neighbour's.

The game builds its Direct3D headers from the node fields at load (ndVertexBuffer2InitNode, ndPushBufferInitNode), so
a rebuilt buffer needs only its data, its size or primitive count, and the pointer to it. Draws are written as
triangle strips with 16-bit indices, as every shipped model's are, since the game actually draws from command streams
with the primitive type baked in as a strip; their baked index counts are rewritten too (see _baked_draws). The .gpu
entry holding the buffers is laid out again from scratch.
"""

from __future__ import annotations

import base64
import json
import math
import os
import re
import struct
from dataclasses import dataclass, field, replace

import numpy as np

from .caff import Caff
from .extend import Added, NewDraw, add_draws
from .graph import Node, Ref, Resolver, rendergraph_roots
from .mesh import BINORMAL, BLENDINDICES, BLENDWEIGHT, COLOR, NORMAL, POSITION, TANGENT, TEXCOORD, Draw, Element, Mesh, collect_meshes, decode_element, encode_element, has_second_uvs, read_declaration
from .material import read_material, texture_palette
from .skeleton import Skeleton, read_skeleton
from .texture import build_texture, read_texture, to_pixels

PRIMITIVE_TRIANGLE_STRIP = 6
BUFFER_ALIGNMENT = 32
# A copy of a draw's material, as Blender names a duplicate (graph0_mesh5_draw2.001), is a new draw beside it.
DRAW_NAME = re.compile(r"graph(\d+)_mesh(\d+)_draw(\d+)(?:\.(\d+))?")
# The whole name (a Blender duplicate's ".001" allowed): an FBX export's leaf bones, joint_05_end, are not joints.
JOINT_NAME = re.compile(r"joint_(\d+)(?:\.\d+)?$")


class ImportError_(Exception):
    pass


# --- glTF reading -----------------------------------------------------------------------------------------------

COMPONENTS = {5120: ("b", 1, 127.0), 5121: ("B", 1, 255.0), 5122: ("h", 2, 32767.0), 5123: ("H", 2, 65535.0), 5125: ("I", 4, None), 5126: ("f", 4, None)}
WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


class Gltf:
    def __init__(self, path: str):
        folder = os.path.dirname(os.path.abspath(path))
        self.folder = folder
        embedded = b""
        with open(path, "rb") as handle:
            blob = handle.read()
        if blob[:4] == b"glTF":
            length = struct.unpack_from("<I", blob, 8)[0]
            cursor = 12
            while cursor < length:
                size, kind = struct.unpack_from("<II", blob, cursor)
                chunk = blob[cursor + 8:cursor + 8 + size]
                if kind == 0x4E4F534A:
                    self.doc = json.loads(chunk)
                elif kind == 0x004E4942:
                    embedded = chunk
                cursor += 8 + size
        else:
            self.doc = json.loads(blob.decode("utf-8"))
        self.buffers = []
        for buffer in self.doc.get("buffers", []):
            uri = buffer.get("uri")
            if uri is None:
                self.buffers.append(embedded)
            elif uri.startswith("data:"):
                self.buffers.append(base64.b64decode(uri.split(",", 1)[1]))
            else:
                with open(os.path.join(folder, uri.replace("%20", " ")), "rb") as handle:
                    self.buffers.append(handle.read())
        self.world = self._world_matrices()

    def image(self, index: int):
        """An image's pixels, (h, w, 4) bytes, from its file, data URI or buffer view."""
        from .encode import read_png, read_png_bytes

        image = self.doc["images"][index]
        if "bufferView" in image:
            view = self.doc["bufferViews"][image["bufferView"]]
            start = view.get("byteOffset", 0)
            return read_png_bytes(self.buffers[view["buffer"]][start:start + view["byteLength"]])
        uri = image["uri"]
        if uri.startswith("data:"):
            return read_png_bytes(base64.b64decode(uri.split(",", 1)[1]))
        return read_png(os.path.join(self.folder, uri.replace("%20", " ")))

    def material_image(self, material: dict, slot: str) -> int | None:
        """The image a material's base colour ("colour") or normal ("normal") texture shows, if it has one."""
        info = material.get("pbrMetallicRoughness", {}).get("baseColorTexture") if slot == "colour" else material.get("normalTexture")
        if info is None:
            return None
        return self.doc["textures"][info["index"]].get("source")

    def accessor(self, index: int) -> list[tuple[float, ...]]:
        accessor = self.doc["accessors"][index]
        fmt, size, divisor = COMPONENTS[accessor["componentType"]]
        width = WIDTHS[accessor["type"]]
        count = accessor["count"]
        if "bufferView" not in accessor:
            return [(0.0,) * width] * count
        view = self.doc["bufferViews"][accessor["bufferView"]]
        stride = view.get("byteStride") or size * width
        base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        data = self.buffers[view["buffer"]]
        out = []
        for i in range(count):
            values = struct.unpack_from(f"<{width}{fmt}", data, base + i * stride)
            if accessor.get("normalized") and divisor:
                values = tuple(max(v / divisor, -1.0) for v in values)
            out.append(values)
        return out

    def _world_matrices(self) -> list[list[float]]:
        nodes = self.doc.get("nodes", [])
        parent = {}
        for i, node in enumerate(nodes):
            for child in node.get("children", []):
                parent[child] = i
        local = [_node_matrix(node) for node in nodes]
        world: list[list[float] | None] = [None] * len(nodes)

        def resolve(i: int) -> list[float]:
            if world[i] is None:
                world[i] = _mul(resolve(parent[i]), local[i]) if i in parent else local[i]
            return world[i]

        return [resolve(i) for i in range(len(nodes))]


# 4x4 matrices as 16 floats, column-major as glTF stores them.
def _mul(a: list[float], b: list[float]) -> list[float]:
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def _node_matrix(node: dict) -> list[float]:
    if "matrix" in node:
        return list(node["matrix"])
    tx, ty, tz = node.get("translation", (0.0, 0.0, 0.0))
    x, y, z, w = node.get("rotation", (0.0, 0.0, 0.0, 1.0))
    sx, sy, sz = node.get("scale", (1.0, 1.0, 1.0))
    return [
        (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0.0,
        (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0.0,
        (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0.0,
        tx, ty, tz, 1.0,
    ]


def _point(m: list[float], v: tuple[float, ...]) -> tuple[float, float, float]:
    return tuple(m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] for r in range(3))


def _vector(m: list[float], v: tuple[float, ...]) -> tuple[float, float, float]:
    return tuple(m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] for r in range(3))


def _normalised(v: tuple[float, ...]) -> tuple[float, float, float] | None:
    length = math.sqrt(sum(c * c for c in v[:3]))
    return tuple(c / length for c in v[:3]) if length > 1e-9 else None


def _unflip(v: tuple[float, ...]) -> tuple[float, float, float]:
    return (-v[0], v[1], v[2])


# --- what the glTF asks for -----------------------------------------------------------------------------------------

@dataclass
class Vertex:
    position: tuple[float, float, float]  # game model space
    normal: tuple[float, float, float] | None
    uv: tuple[float, float] | None
    colour: tuple[float, float, float, float] | None  # glTF RGBA
    second_uv: tuple[float, float] | None
    skin: list[tuple[int, float]] | None  # (game joint, weight)


def _bind_matrices(gltf: Gltf, skin: dict) -> tuple[list[float], list[int]]:
    """Per skin joint, its world matrix times its inverse bind matrix, and the game joint it names."""
    inverse = gltf.accessor(skin["inverseBindMatrices"]) if "inverseBindMatrices" in skin else [(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)] * len(skin["joints"])
    matrices, joints = [], []
    for node_index, ibm in zip(skin["joints"], inverse):
        name = gltf.doc["nodes"][node_index].get("name", "")
        match = JOINT_NAME.match(name)
        if not match and name.endswith("_end"):
            # An FBX export's leaf bone: nothing is weighted to it.
            matrices.append([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
            joints.append(-1)
            continue
        if not match:
            raise ImportError_(f"bone '{name}' is not one of the game's joints (joint_<number>); the game's skeleton cannot gain bones")
        matrices.append(_mul(gltf.world[node_index], list(ibm)))
        joints.append(int(match.group(1)))
    return matrices, joints


def draw_key(match) -> tuple[int, int, int, int]:
    """(graph, mesh, draw, copy) from a DRAW_NAME match; copy 0 is the game's own draw."""
    g, m, d, copy = match.groups()
    return int(g), int(m), int(d), int(copy or 0)


def read_draws(gltf: Gltf) -> tuple[dict[tuple[int, int, int], list[tuple[Vertex, Vertex, Vertex]]], dict[int, dict[int, tuple[float, float, float]]]]:
    """Triangles by the (graph, mesh, draw) their material names, and each glTF skin's joint positions in game space."""
    doc = gltf.doc
    materials = doc.get("materials", [])
    out: dict[tuple[int, int, int], list[tuple[Vertex, Vertex, Vertex]]] = {}
    joint_positions: dict[int, dict[int, tuple[float, float, float]]] = {}
    skins = {}
    for number, skin in enumerate(doc.get("skins", [])):
        skins[number] = _bind_matrices(gltf, skin)
        joint_positions[number] = {joint: _unflip(_point(gltf.world[node], (0.0, 0.0, 0.0))) for node, joint in zip(skin["joints"], skins[number][1]) if joint >= 0}
    for node_index, node in enumerate(doc.get("nodes", [])):
        if "mesh" not in node:
            continue
        skin = skins.get(node.get("skin")) if "skin" in node else None
        world = gltf.world[node_index]
        for primitive in doc["meshes"][node["mesh"]]["primitives"]:
            if primitive.get("mode", 4) != 4:
                raise ImportError_(f"{node.get('name', node_index)}: only triangles can be imported")
            material = materials[primitive["material"]].get("name", "") if "material" in primitive else ""
            match = DRAW_NAME.search(material)
            if not match:
                raise ImportError_(f"{node.get('name', node_index)} has triangles with material '{material or 'none'}', which names no draw; give them one of the graph<g>_mesh<m>_draw<d> materials")
            attributes = primitive["attributes"]
            positions = gltf.accessor(attributes["POSITION"])
            normals = gltf.accessor(attributes["NORMAL"]) if "NORMAL" in attributes else None
            uvs = gltf.accessor(attributes["TEXCOORD_0"]) if "TEXCOORD_0" in attributes else None
            second_uvs = gltf.accessor(attributes["TEXCOORD_1"]) if "TEXCOORD_1" in attributes else None
            colours = gltf.accessor(attributes["COLOR_0"]) if "COLOR_0" in attributes else None
            joints = gltf.accessor(attributes["JOINTS_0"]) if skin and "JOINTS_0" in attributes else None
            weights = gltf.accessor(attributes["WEIGHTS_0"]) if joints else None
            vertices = []
            for i, p in enumerate(positions):
                influences = None
                if joints:
                    influences = [(skin[1][int(j)], w) for j, w in zip(joints[i], weights[i]) if w > 0.0 and skin[1][int(j)] >= 0]
                    total = sum(w for _, w in influences)
                    if total > 0.0:
                        m = [sum(w * skin[0][int(j)][k] for j, w in zip(joints[i], weights[i]) if w > 0.0) / total for k in range(16)]
                    else:
                        influences, m = None, world
                else:
                    m = world
                n = _normalised(_vector(m, normals[i])) if normals else None
                colour = tuple(colours[i]) + (1.0,) * (4 - len(colours[i])) if colours else None
                vertices.append(Vertex(_unflip(_point(m, p)), _unflip(n) if n else None, (uvs[i][0], uvs[i][1]) if uvs else None, colour, (second_uvs[i][0], second_uvs[i][1]) if second_uvs else None, influences))
            indices = [i[0] for i in gltf.accessor(primitive["indices"])] if "indices" in primitive else list(range(len(positions)))
            target = out.setdefault(draw_key(match), [])
            # The export reversed the winding with the mirrored X; reverse it back.
            for t in range(0, len(indices) - 2, 3):
                target.append((vertices[int(indices[t])], vertices[int(indices[t + 2])], vertices[int(indices[t + 1])]))
    return out, joint_positions


# --- the archive side ---------------------------------------------------------------------------------------------

@dataclass
class Stream:
    record: Ref  # the 20-byte stream record
    stride: int
    data: Ref | None
    size: int
    elements: list[Element] = field(default_factory=list)  # every variant's elements in this stream, offsets within the stride


@dataclass
class Variant:
    ref: Ref
    position_scale: bool  # whether it has a c_posscale register
    uv_scale: bool


def read_layout(resolver: Resolver, node: Node) -> tuple[list[Stream], list[Variant]]:
    table = resolver.pointer(node.ref, 48)
    streams = []
    for i in range(resolver.u32(node.ref, 56)):
        record = Ref(table.entry, table.offset + 20 * i)
        streams.append(Stream(record, resolver.u32(record, 0), resolver.pointer(record, 12), resolver.u32(record, 16)))
    variants = []
    declarations = []
    variant_set = resolver.pointer(node.ref, 52)
    for i in range(resolver.u32(variant_set, 0)):
        variant = resolver.pointer(resolver.pointer(variant_set, 4), 4 * i)
        variants.append(Variant(variant, resolver.u16(variant, 18) != 0xFFFF, resolver.u16(variant, 20) != 0xFFFF))
        declarations.append((resolver.pointer(variant, 0), read_declaration(resolver, resolver.pointer(variant, 28))))
    # A variant numbers its TEXCOORDs from its own first one, so one without UVs calls the first blend-shape stream
    # TEXCOORD0; each place in a stream takes its meaning from the fullest variant that reads it.
    for stream_indices, elements in sorted(declarations, key=lambda d: -len(d[1])):
        for element in elements:
            record = resolver.u8(stream_indices, element.stream) if stream_indices else element.stream
            known = streams[record].elements
            if not any(e.offset == element.offset for e in known):
                known.append(element)
    return streams, variants


def _index_bytes(primitive: int, count: int) -> int:
    """ndPushBufferInitNode's index buffer size."""
    return {4: 6 * count, 5: 2 * (count + 2), 6: 2 * (count + 2), 13: 8 * count}.get(primitive, 0)


class Grid:
    """Nearest-point lookup over a fixed set of points."""

    def __init__(self, points: list[tuple[float, float, float]], ids: list[int]):
        self.points, self.ids = points, ids
        lo = [min(p[k] for p in points) for k in range(3)]
        hi = [max(p[k] for p in points) for k in range(3)]
        self.cell = max(max(hi[k] - lo[k] for k in range(3)) / 48.0, 1e-6)
        self.cells: dict[tuple[int, int, int], list[int]] = {}
        for i, p in enumerate(points):
            self.cells.setdefault(self._key(p), []).append(i)

    def _key(self, p) -> tuple[int, int, int]:
        return tuple(int(math.floor(c / self.cell)) for c in p)

    def within(self, p, radius: float) -> list[int]:
        reach = int(math.ceil(radius / self.cell))
        if (2 * reach + 1) ** 3 > len(self.points):
            # Points bunched far tighter than the radius (a draw whose UVs all sit at one spot): checking each is quicker.
            return [self.ids[i] for i, q in enumerate(self.points) if (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 + (q[2] - p[2]) ** 2 <= radius * radius]
        cx, cy, cz = self._key(p)
        out = []
        for x in range(cx - reach, cx + reach + 1):
            for y in range(cy - reach, cy + reach + 1):
                for z in range(cz - reach, cz + reach + 1):
                    for i in self.cells.get((x, y, z), ()):
                        q = self.points[i]
                        if (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 + (q[2] - p[2]) ** 2 <= radius * radius:
                            out.append(self.ids[i])
        return out

    def nearest(self, p) -> int:
        cx, cy, cz = self._key(p)
        best, best_d = None, float("inf")
        for ring in range(4):
            for x in range(cx - ring, cx + ring + 1):
                for y in range(cy - ring, cy + ring + 1):
                    for z in range(cz - ring, cz + ring + 1):
                        if max(abs(x - cx), abs(y - cy), abs(z - cz)) != ring:
                            continue
                        for i in self.cells.get((x, y, z), ()):
                            q = self.points[i]
                            d = (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 + (q[2] - p[2]) ** 2
                            if d < best_d:
                                best, best_d = i, d
            # Anything outside the rings searched is at least `ring` cells away.
            if best is not None and best_d <= (ring * self.cell) ** 2:
                return self.ids[best]
        # Far from every point (a vertex moved well away): look at them all.
        best = min(range(len(self.points)), key=lambda i: (self.points[i][0] - p[0]) ** 2 + (self.points[i][1] - p[1]) ** 2 + (self.points[i][2] - p[2]) ** 2)
        return self.ids[best]


class Edits:
    """Entry contents as they are being changed."""

    def __init__(self, resolver: Resolver):
        self.resolver = resolver
        self.data: dict[int, bytearray] = {}

    def entry(self, index: int) -> bytearray:
        if index not in self.data:
            self.data[index] = bytearray(self.resolver.data(self.resolver.archive.entries[index - 1]))
        return self.data[index]

    def u32(self, ref: Ref, offset: int, value: int) -> None:
        struct.pack_into(">I", self.entry(ref.entry.index), ref.offset + offset, value)

    def f32(self, ref: Ref, offset: int, value: float) -> None:
        struct.pack_into(">f", self.entry(ref.entry.index), ref.offset + offset, value)

    def u8(self, ref: Ref, offset: int, value: int) -> None:
        self.entry(ref.entry.index)[ref.offset + offset] = value


@dataclass
class Target:
    graph: int
    number: int
    mesh: Mesh
    skeleton: Skeleton | None
    streams: list[Stream]
    variants: list[Variant]


def import_gltf(original: str, gltf_path: str, out: str, keep_missing: bool = False) -> list[str]:
    archive = Caff.load(original)
    gltf = Gltf(gltf_path)
    named, joint_positions = read_draws(gltf)
    notes: list[str] = []

    # Draws the glTF adds (copies of a game draw's material) are made first; every draw is then known by its push
    # buffer and slot there, which adding draws does not move, and numbered again from the grown archive.
    resolver = Resolver(archive)
    identity_of: dict[tuple[int, int, int, int], tuple[int, int]] = {}
    budgets: dict[tuple[int, int, int], tuple[int, Skeleton | None]] = {}
    for graph, entry in enumerate(e for e in archive.entries if resolver.type_name(e) == "rendergraph"):
        roots = rendergraph_roots(resolver, entry)
        skeleton = read_skeleton(resolver, roots)
        for number, mesh in enumerate(collect_meshes(resolver, roots)):
            for d, draw in enumerate(mesh.draws):
                identity_of[(graph, number, d, 0)] = (draw.push.ref.offset, draw.slot)
                budgets[(graph, number, d)] = (len(draw.palette), skeleton if (BLENDINDICES, 0) in mesh.attributes else None)
    materials = {draw_key(m): material for material in gltf.doc.get("materials", []) for m in [DRAW_NAME.search(material.get("name", ""))] if m}
    named = _split_by_bones(named, budgets, materials, notes)
    new_draws = []
    for key in sorted(k for k in set(named) | set(materials) if k[3]):
        if key[:3] + (0,) not in identity_of:
            raise ImportError_(f"graph{key[0]}_mesh{key[1]}_draw{key[2]}.{key[3]:03d} copies a draw the model does not have")
        images = {slot: gltf.material_image(materials[key], slot) for slot in ("colour", "normal")} if key in materials else {}
        new_draws.append(NewDraw(key, {slot: image for slot, image in images.items() if image is not None}))
    added = Added()
    if new_draws:
        def images_of_template(key):
            material = materials.get(key + (0,))
            return {slot: gltf.material_image(material, slot) for slot in ("colour", "normal")} if material else {}

        archive, added = add_draws(archive, new_draws, images_of_template)
        identity_of.update(added.identity)
        notes.append(f"added {len(new_draws)} draw{'s' if len(new_draws) > 1 else ''}: " + ", ".join(f"graph{k[0]}_mesh{k[1]}_draw{k[2]}.{k[3]:03d}" for k in sorted(added.identity)))
    resolver = Resolver(archive)
    numbered: dict[tuple[int, int], tuple[int, int, int]] = {}
    for graph, entry in enumerate(e for e in archive.entries if resolver.type_name(e) == "rendergraph"):
        for number, mesh in enumerate(collect_meshes(resolver, rendergraph_roots(resolver, entry))):
            for d, draw in enumerate(mesh.draws):
                numbered[(draw.push.ref.offset, draw.slot)] = (graph, number, d)
    key_of = {key: numbered[identity] for key, identity in identity_of.items() if identity in numbered}
    stray = sorted(k for k in named if k not in key_of)
    if stray:
        raise ImportError_("materials name draws the model does not have: " + ", ".join(f"graph{g}_mesh{m}_draw{d}" for g, m, d, _ in stray))
    wanted = {}
    for key, triangles in named.items():
        wanted.setdefault(key_of[key], []).extend(triangles)
    palette_lists = {numbered[i]: Ref(archive.entries[e - 1], o) for i, (e, o) in added.palettes.items()}
    extra_textures = {numbered[i]: {slot: Ref(archive.entries[e - 1], o) for slot, (e, o) in slots.items()} for i, slots in added.textures.items()}

    targets: list[Target] = []
    skeletons = []
    for graph, entry in enumerate(e for e in archive.entries if resolver.type_name(e) == "rendergraph"):
        roots = rendergraph_roots(resolver, entry)
        skeleton = read_skeleton(resolver, roots)
        for number, mesh in enumerate(collect_meshes(resolver, roots)):
            streams, variants = read_layout(resolver, mesh.node)
            targets.append(Target(graph, number, mesh, skeleton, streams, variants))
        if skeleton is not None:
            skeletons.append(skeleton)
    for target in targets:
        for d, draw in enumerate(target.mesh.draws):
            if (target.graph, target.number, d) in palette_lists:
                draw.palette_ref = palette_lists[(target.graph, target.number, d)]
                draw.palette = [resolver.u8(draw.palette_ref, i) for i in range(len(draw.palette))]

    edits = Edits(resolver)
    for skeleton in skeletons:
        _move_bones(edits, skeleton, joint_positions, notes)
    baked = _baked_draws(resolver)
    new_blocks: dict[tuple[int, int], bytes] = {}  # (entry, pointer field offset) -> the new data it points at
    palettes: dict[tuple[int, int], dict] = {}
    palette_of: dict[int, dict] = {}  # id(draw) -> its palette
    rebuilt = []
    for target in targets:
        mesh = target.mesh
        draws_named = [d for d in range(len(mesh.draws)) if (target.graph, target.number, d) in wanted]
        if not draws_named and keep_missing:
            continue
        hidden = [d for d in range(len(mesh.draws)) if d not in draws_named]
        if hidden:
            notes.append(f"graph{target.graph}_mesh{target.number}: hid draw{'s' if len(hidden) > 1 else ''} " + ", ".join(str(d) for d in hidden) + " (no triangles in the glTF)")
        rebuilt.append((target, [wanted.get((target.graph, target.number, d), []) for d in range(len(mesh.draws))]))
        for draw in mesh.draws:
            if draw.skin is not None:
                first = draw.palette_ref or resolver.pointer(draw.skin.ref, 48)
                palette = palettes.setdefault((first.entry.index, first.offset), {"ref": first, "old": draw.palette, "weights": {}})
                palette_of[id(draw)] = palette

    # Skin first: every vertex's influences as matrix indices, so each palette can be chosen from what its draws use.
    skins: dict[int, list[list[list[tuple[int, float]] | None]]] = {}
    for target, triangle_sets in rebuilt:
        skins[id(target)] = [_draw_skins(target, d, triangles, palette_of.get(id(target.mesh.draws[d]))) for d, triangles in enumerate(triangle_sets)]
    for palette in palettes.values():
        palette["new"], palette["kept"] = _choose_palette(palette, notes)
        for slot, matrix in enumerate(palette["new"]):
            if matrix != palette["old"][slot]:
                edits.u8(palette["ref"], slot, matrix)

    for target, triangle_sets in rebuilt:
        _rebuild_mesh(resolver, edits, target, triangle_sets, skins[id(target)], palette_of, new_blocks, baked, notes)

    _import_textures(resolver, edits, gltf, notes, key_of, extra_textures)
    replaced = _relayout(resolver, edits, new_blocks)
    with open(out, "wb") as handle:
        handle.write(archive.write(replaced))
    return notes


def _move_bones(edits: Edits, skeleton: Skeleton, joint_positions: dict[int, dict[int, tuple]], notes: list[str]) -> None:
    """Writes bones moved in the glTF into the skeleton: a moved joint's pivot (its bind position) becomes its new
    position, and its translation from its parent shifts by how far it moved beyond how far its parent did, keeping any
    difference the game had between the two. Animations add their offsets to that translation, so they still play.

    The glTF skin that is this skeleton is the one whose joints mostly sit where this skeleton's do."""
    def moved_by(positions: dict[int, tuple], j: int) -> tuple[float, float, float]:
        if j < 0 or j not in positions:
            return (0.0, 0.0, 0.0)
        return tuple(positions[j][k] - skeleton.joints[j].pivot[k] for k in range(3))

    best, best_still = None, -1
    for positions in joint_positions.values():
        if any(j >= len(skeleton.joints) for j in positions):
            continue
        still = sum(1 for j in positions if max(abs(c) for c in moved_by(positions, j)) <= 1e-3)
        if still > best_still:
            best, best_still = positions, still
    if best is None or skeleton.array is None:
        return
    moved = [j for j in best if max(abs(c) for c in moved_by(best, j)) > 1e-3]
    for j in moved:
        joint = skeleton.joints[j]
        own, parent = moved_by(best, j), moved_by(best, joint.parent)
        for k in range(3):
            edits.f32(skeleton.array, 36 * j + 4 * k, joint.translation[k] + own[k] - parent[k])
            edits.f32(skeleton.array, 36 * j + 12 + 4 * k, joint.pivot[k] + own[k])
    if moved:
        notes.append(f"moved {len(moved)} bone{'s' if len(moved) > 1 else ''} to where the glTF has them")


def _split_by_bones(named: dict, budgets: dict, materials: dict, notes: list[str]) -> dict:
    """Triangles a draw's bone palette cannot hold, split off into copies of that draw.

    A draw's palette holds as many skinning matrices as the vertex shader has room for (the game's own palette length,
    12 at most). Where a draw's triangles use more, they are grouped, first fit in bone order so neighbouring bones
    share a group, each group within the palette; the first group keeps the draw and every other becomes a copy
    (a new draw showing the same textures). Weights too small to survive as a byte are ignored."""
    out: dict = {}
    for key, triangles in named.items():
        capacity, skeleton = budgets.get(key[:3], (0, None))
        if skeleton is None or capacity == 0:
            out[key] = triangles
            continue

        def matrices(triangle) -> frozenset:
            found = set()
            for vertex in triangle:
                for joint, weight in vertex.skin or ():
                    if weight > 0.5 / 255:
                        matrix = _matrix_of_joint(skeleton, joint)
                        if matrix is not None:
                            found.add(matrix)
            return frozenset(found)

        needs = [(matrices(t), t) for t in triangles]
        if len(frozenset().union(*(m for m, _ in needs))) <= capacity:
            out[key] = triangles
            continue
        groups: list[tuple[set, list]] = []
        for used, triangle in sorted(needs, key=lambda n: sorted(n[0])):
            for bones, members in groups:
                if len(bones | used) <= capacity:
                    bones |= used
                    members.append(triangle)
                    break
            else:
                groups.append((set(used), [triangle]))
        out[key] = groups[0][1]
        taken = {k[3] for k in list(named) + list(out) if k[:3] == key[:3]}
        copy = max(taken) + 1
        for _, members in groups[1:]:
            while copy in taken:
                copy += 1
            new_key = key[:3] + (copy,)
            taken.add(copy)
            out[new_key] = members
            if key in materials:
                materials[new_key] = materials[key]
        notes.append(f"graph{key[0]}_mesh{key[1]}_draw{key[2]}{f'.{key[3]:03d}' if key[3] else ''} follows more bones than one draw can ({capacity}): split into {len(groups)} draws")
    return out


def _matrix_of_joint(skeleton: Skeleton, joint: int) -> int | None:
    """The skinning matrix that follows `joint`, or its nearest ancestor's when it has none."""
    seen = set()
    while joint >= 0 and joint not in seen:
        seen.add(joint)
        if joint in skeleton.matrix_joints:
            return skeleton.matrix_joints.index(joint)
        joint = skeleton.joints[joint].parent if joint < len(skeleton.joints) else -1
    return None


def _old_influences(target: Target, draw: Draw, vertex: int) -> list[tuple[int, float]]:
    """An old vertex's influences as (matrix, weight)."""
    index = target.mesh.attributes.get((BLENDINDICES, 0))
    if not index or not draw.palette:
        return []
    weight = target.mesh.attributes.get((BLENDWEIGHT, 0))
    out = []
    for k in range(4):
        w = weight[vertex][3 - k] if weight else (1.0 if k == 0 else 0.0)
        slot = int(index[vertex][k]) // 3
        if w > 0.0 and slot < len(draw.palette):
            out.append((draw.palette[slot], w))
    return out


@dataclass
class DrawLookup:
    """A draw's old vertices by position, and by UV when it has them."""
    position: Grid
    uv: Grid | None


def _draw_grid(target: Target, draw: Draw) -> DrawLookup:
    positions = target.mesh.positions
    used = sorted({v for t in draw.triangles() for v in t if v < len(positions)}) or list(range(len(positions)))
    uvs = target.mesh.uvs() if (TEXCOORD, 0) in target.mesh.attributes else None
    return DrawLookup(Grid([positions[v] for v in used], used), Grid([(uvs[v][0], uvs[v][1], 0.0) for v in used], used) if uvs else None)


def _quantised(position: tuple[float, float, float], scale: float) -> tuple[int, int, int]:
    return tuple(round(c / scale) for c in position)


def _corner_key(position: tuple[int, ...], uv: tuple[int, ...] | None) -> tuple:
    return (position, uv)


def _old_triangles(target: Target, draw: Draw) -> dict[tuple, list[tuple[int, int, int]]]:
    """The draw's old triangles by their corners' quantised positions and UVs, in each of their three rotations; a
    triangle drawn twice over the same corners is listed twice, so each copy finds its own."""
    positions = target.mesh.attributes.get((POSITION, 0), [])
    uvs = target.mesh.attributes.get((TEXCOORD, 0))
    out: dict[tuple, list[tuple[int, int, int]]] = {}
    for a, b, c in draw.triangles():
        if max(a, b, c) >= len(positions):
            continue
        for corners in ((a, b, c), (b, c, a), (c, a, b)):
            key = tuple(_corner_key(tuple(int(v) for v in positions[i][:3]), tuple(int(v) for v in uvs[i][:2]) if uvs else None) for i in corners)
            out.setdefault(key, []).append(corners)
    return out


def _same_vertex(target: Target, old: int, vertex: Vertex) -> bool:
    """Whether a new vertex is the old one unmoved: position and UVs to the stored precision, normal to a hair."""
    mesh = target.mesh
    if any(abs(vertex.position[k] - mesh.positions[old][k]) > mesh.position_scale for k in range(3)):
        return False
    uvs = mesh.attributes.get((TEXCOORD, 0))
    if vertex.uv and uvs and any(abs(vertex.uv[k] - uvs[old][k] * mesh.uv_scale) > mesh.uv_scale for k in range(2)):
        return False
    normals = mesh.attributes.get((NORMAL, 0))
    if vertex.normal and normals:
        n = _normalised(normals[old])
        # Within 0.8 degrees: the stored normal is 10-bit, and Blender hands normals back a little off.
        if n is None or sum(n[k] * vertex.normal[k] for k in range(3)) < 0.9999:
            return False
    return True


def _old_vertex(target: Target, lookup: DrawLookup, vertex: Vertex) -> int:
    """The old vertex a new one starts from: of those where it stands, the one whose UV and normal agree best (a hard
    edge or UV seam keeps several there). A vertex that moved is found by its UV, which reshaping leaves alone (of
    several there, the nearest), and only failing that by position: matching a moved face by position would hand it
    another part's blend-shape deltas."""
    mesh = target.mesh
    here = lookup.position.within(vertex.position, 2 * mesh.position_scale)
    if not here:
        if vertex.uv and lookup.uv:
            same_uv = lookup.uv.within((vertex.uv[0], vertex.uv[1], 0.0), 2 * mesh.uv_scale)
            if same_uv:
                positions = mesh.positions
                return min(same_uv, key=lambda v: sum((positions[v][k] - vertex.position[k]) ** 2 for k in range(3)))
        return lookup.position.nearest(vertex.position)
    uvs = mesh.uvs() if vertex.uv else None
    second_uvs = mesh.uvs(1) if vertex.second_uv and has_second_uvs(mesh) else None
    normals = mesh.attributes.get((NORMAL, 0)) if vertex.normal else None

    def mismatch(v: int) -> float:
        score = 0.0
        if uvs:
            score += (uvs[v][0] - vertex.uv[0]) ** 2 + (uvs[v][1] - vertex.uv[1]) ** 2
        if second_uvs:
            score += (second_uvs[v][0] - vertex.second_uv[0]) ** 2 + (second_uvs[v][1] - vertex.second_uv[1]) ** 2
        if normals:
            n = _normalised(normals[v]) or (0.0, 0.0, 0.0)
            score += 1.0 - sum(n[k] * vertex.normal[k] for k in range(3))
        return score

    return min(here, key=mismatch)


def _draw_skins(target: Target, d: int, triangles, palette: dict | None) -> list[list[tuple[int, float]] | None]:
    """Per triangle corner, its influences as (matrix, weight); also adds them to the palette's totals."""
    mesh, draw = target.mesh, target.mesh.draws[d]
    if (BLENDINDICES, 0) not in mesh.attributes or palette is None or target.skeleton is None:
        return [None] * (3 * len(triangles))
    grid = None
    out = []
    for triangle in triangles:
        for vertex in triangle:
            if vertex.skin:
                influences = []
                for joint, weight in vertex.skin:
                    matrix = _matrix_of_joint(target.skeleton, joint)
                    if matrix is not None:
                        influences.append((matrix, weight))
            else:
                grid = grid or _draw_grid(target, draw)
                influences = _old_influences(target, draw, _old_vertex(target, grid, vertex))
            for matrix, weight in influences:
                palette["weights"][matrix] = palette["weights"].get(matrix, 0.0) + weight
            out.append(influences)
    return out


def _choose_palette(palette: dict, notes: list[str]) -> tuple[list[int], set[int]]:
    """The palette's new matrices, the same length as the old, and the set of matrices kept in it."""
    old: list[int] = palette["old"]
    weights: dict[int, float] = palette["weights"]
    used = sorted(weights, key=lambda m: -weights[m])
    if len(used) > len(old):
        notes.append(f"a bone palette holds {len(old)} bones but its draws use {len(used)}; the {len(used) - len(old)} lightest follow their nearest kept parent")
        used = used[:len(old)]
    kept = set(used) or set(old)
    # A kept matrix stays in its old slot where it had one, so an unchanged model writes its palette unchanged.
    new = [m if m in kept else None for m in old]
    spare = [m for m in used if m not in new]
    for slot in range(len(new)):
        if new[slot] is None:
            new[slot] = spare.pop(0) if spare else next(m for m in new if m is not None)
    return new, kept


def _remap_matrix(skeleton: Skeleton, matrix: int, kept: set[int]) -> int:
    """`matrix`, or the matrix of its joint's nearest ancestor among `kept`, or failing that the kept one nearest it."""
    if matrix in kept:
        return matrix
    joint = skeleton.joint_of_matrix(matrix)
    seen = set()
    walk = skeleton.joints[joint].parent
    while walk >= 0 and walk not in seen:
        seen.add(walk)
        for m in kept:
            if skeleton.joint_of_matrix(m) == walk:
                return m
        walk = skeleton.joints[walk].parent
    pivot = skeleton.joints[joint].pivot
    return min(kept, key=lambda m: sum((skeleton.joints[skeleton.joint_of_matrix(m)].pivot[k] - pivot[k]) ** 2 for k in range(3)))


def _skin_values(influences: list[tuple[int, float]], palette: list[int]) -> tuple[list[int], list[int]]:
    """Four palette slots and their weights in 255ths, heaviest first, the weights summing to 255."""
    by_slot: dict[int, float] = {}
    for matrix, weight in influences:
        slot = palette.index(matrix)
        by_slot[slot] = by_slot.get(slot, 0.0) + weight
    ranked = sorted(by_slot.items(), key=lambda item: -item[1])[:4] or [(0, 1.0)]
    total = sum(w for _, w in ranked) or 1.0
    weights = [round(255 * w / total) for _, w in ranked]
    weights[0] += 255 - sum(weights)
    slots = [s for s, _ in ranked]
    while len(slots) < 4:
        slots.append(slots[0])
        weights.append(0)
    return slots, weights


def _tangents(positions, uvs, normals, triangles) -> tuple[list, list]:
    """Per vertex a tangent along +U and a binormal along -V, each made perpendicular to the normal."""
    tangent = [[0.0, 0.0, 0.0] for _ in positions]
    binormal = [[0.0, 0.0, 0.0] for _ in positions]
    for a, b, c in triangles:
        if uvs[a] is None or uvs[b] is None or uvs[c] is None:
            continue
        e1 = [positions[b][k] - positions[a][k] for k in range(3)]
        e2 = [positions[c][k] - positions[a][k] for k in range(3)]
        du1, dv1 = uvs[b][0] - uvs[a][0], uvs[b][1] - uvs[a][1]
        du2, dv2 = uvs[c][0] - uvs[a][0], uvs[c][1] - uvs[a][1]
        det = du1 * dv2 - du2 * dv1
        if abs(det) < 1e-12:
            continue
        for k in range(3):
            t = (e1[k] * dv2 - e2[k] * dv1) / det
            bn = -(e2[k] * du1 - e1[k] * du2) / det
            for v in (a, b, c):
                tangent[v][k] += t
                binormal[v][k] += bn
    out_t, out_b = [], []
    for v in range(len(positions)):
        n = normals[v]
        pair = []
        for axis in (tangent[v], binormal[v]):
            if n is not None:
                d = sum(axis[k] * n[k] for k in range(3))
                axis = [axis[k] - d * n[k] for k in range(3)]
            pair.append(_normalised(axis))
        out_t.append(pair[0])
        out_b.append(pair[1])
    return out_t, out_b


ELEMENT_SIZES = {0x2A2187: 4, 0x1A2086: 4, 0x2C2359: 4, 0x1A235A: 8}


def _wrap_uvs(triangle, limit: float):
    """(the triangle with each UV set moved by whole tiles to centre on the first tile, when it reaches beyond `limit`,
    or the triangle itself when it does not; whether a UV still beyond had to be clamped, the triangle spanning more
    tiles than the limit holds)."""
    def fits(uvs) -> bool:
        return all(uv is None or all(abs(c) <= limit for c in uv[:2]) for uv in uvs)

    changes = {}
    clamped = False
    for attribute in ("uv", "second_uv"):
        uvs = [getattr(v, attribute) for v in triangle]
        if fits(uvs):
            continue
        shift = [math.floor(sum(uv[k] for uv in uvs) / 3) for k in range(2)]
        moved = [(uv[0] - shift[0], uv[1] - shift[1]) + tuple(uv[2:]) for uv in uvs]
        if not fits(moved):
            clamped = True
            moved = [tuple(max(-limit, min(limit, c)) for c in uv[:2]) + tuple(uv[2:]) for uv in moved]
        changes[attribute] = moved
    if not changes:
        return triangle, False
    return [replace(v, **{attribute: uvs[c] for attribute, uvs in changes.items()}) for c, v in enumerate(triangle)], clamped


def _rebuild_mesh(resolver: Resolver, edits: Edits, target: Target, triangle_sets, skin_sets, palette_of, new_blocks, baked, notes) -> None:
    mesh = target.mesh
    name = f"graph{target.graph}_mesh{target.number}"
    old_count = len(mesh.positions)
    for stream in target.streams:
        if stream.data and stream.stride and stream.size // stream.stride != old_count:
            raise ImportError_(f"{name}: a vertex stream of {stream.size // stream.stride} vertices beside {old_count}; this layout is not understood")
    old_bytes = [resolver.bytes(s.data, s.size) if s.data else b"" for s in target.streams]
    blend_shape = mesh.node.type == "ndBlendShape2"

    # The 16-bit positions and UVs are scaled by c_posscale and c_uvscale, which the game reads from the vertex shader
    # constants baked into the command stream, not from the variant nodes: the scales cannot be raised. UVs beyond their
    # reach are moved by whole tiles, a triangle at a time (the same texels where the texture repeats); a mesh beyond
    # its position reach is refused.
    position_scale, uv_scale = mesh.position_scale, mesh.uv_scale
    reach = max((abs(c) for triangles in triangle_sets for t in triangles for v in t for c in v.position), default=0.0)
    if reach / position_scale > 32767:
        raise ImportError_(f"{name}: the mesh reaches {reach:.2f} from the model's origin, beyond the {32767 * position_scale:.2f} it can hold; keep it closer to the character")
    limit = 32767 * uv_scale
    moved = clamped = 0
    for triangles in triangle_sets:
        for t, triangle in enumerate(triangles):
            wrapped, squeezed = _wrap_uvs(triangle, limit)
            if wrapped is not triangle:
                triangles[t] = wrapped
                moved += 1
                clamped += squeezed
    if moved:
        notes.append(f"{name}: UVs of {moved} triangle{'s' if moved > 1 else ''} moved by whole tiles into the ±{limit:.2f} this mesh can hold")
    if clamped:
        notes.append(f"{name}: {clamped} triangle{'s' if clamped > 1 else ''} span more than {2 * limit:.0f} texture tiles and had their UVs clamped; their texture will look stretched")

    # Vertices, shared by the triangles of a draw that agree on everything written. A triangle the draw had before keeps
    # its old corners as they were: vertices that look alike can differ in what the glTF does not carry, the tangents
    # either side of a mirrored UV seam, or blend-shape deltas.
    vertices: list[tuple] = []  # (Vertex, influences, the old vertex it starts from, its draw)
    index_of: dict[tuple, int] = {}
    draw_indices: list[list[int]] = []
    for d, triangles in enumerate(triangle_sets):
        draw = mesh.draws[d]
        palette = palette_of.get(id(draw))
        grid = _draw_grid(target, draw) if triangles else None
        old_triangles = _old_triangles(target, draw) if triangles else {}
        indices = []
        has_uvs = (TEXCOORD, 0) in mesh.attributes
        for t, triangle in enumerate(triangles):
            key = tuple(_corner_key(_quantised(v.position, mesh.position_scale), tuple(round(c / mesh.uv_scale) for c in v.uv) if has_uvs and v.uv else None) for v in triangle)
            matches = old_triangles.get(key)
            corners = (matches.pop(0) if len(matches) > 1 else matches[0]) if matches else None
            if corners is not None and not all(_same_vertex(target, old, v) for old, v in zip(corners, triangle)):
                corners = None
            for c, vertex in enumerate(triangle):
                influences = skin_sets[d][3 * t + c]
                if influences is not None and palette is not None:
                    influences = [(_remap_matrix(target.skeleton, m, palette["kept"]), w) for m, w in influences]
                old = corners[c] if corners is not None else _old_vertex(target, grid, vertex)
                key = (d, old if corners is not None else None, vertex.position, vertex.normal, vertex.uv, vertex.second_uv, vertex.colour, tuple(influences) if influences else None)
                if key not in index_of:
                    index_of[key] = len(vertices)
                    vertices.append((vertex, influences, old, d))
                indices.append(index_of[key])
        draw_indices.append(indices)
    if len(vertices) > 0xFFFF:
        raise ImportError_(f"{name}: {len(vertices)} vertices, more than 16-bit indices reach (65535)")

    if not vertices:
        # Every draw of the mesh emptied: one old vertex stays, for their degenerate triangles to name.
        vertices.append((Vertex(mesh.positions[0], None, None, None, None, None), None, 0, 0))
    has_tangents = any(e.usage in (TANGENT, BINORMAL) for s in target.streams for e in s.elements)
    tangents, binormals = ([], [])
    if has_tangents:
        triangles = [tuple(ix[i:i + 3]) for ix in draw_indices for i in range(0, len(ix), 3)]
        tangents, binormals = _tangents([v[0].position for v in vertices], [v[0].uv for v in vertices], [v[0].normal for v in vertices], triangles)
    rigid = (BLENDWEIGHT, 0) not in mesh.attributes

    # A vertex that has not moved keeps its old normal, tangent and binormal: the stored normal is not quite unit length,
    # and the old tangents were worked out by the game's own tools.
    unchanged = [_same_vertex(target, old, vertex) for vertex, _, old, _ in vertices]
    growth = _growth(mesh, vertices, draw_indices) if blend_shape else None

    for s, stream in enumerate(target.streams):
        if not stream.data:
            continue
        out = bytearray()
        for number, (vertex, influences, old, d) in enumerate(vertices):
            row = bytearray(old_bytes[s][old * stream.stride:(old + 1) * stream.stride])
            palette = palette_of.get(id(mesh.draws[d]))
            skin = _skin_values(influences, palette["new"]) if influences and palette is not None else None
            for element in stream.elements:
                size = ELEMENT_SIZES.get(element.type)
                if blend_shape and element.usage == TEXCOORD and element.usage_index >= 1 and element.type == 0x1A235A:
                    # A blend shape's offset, scaled as the mesh around the vertex grew or shrank; the fourth value is
                    # not an offset and stays.
                    if growth[number] != 1.0:
                        dx, dy, dz, w = struct.unpack_from(">4h", row, element.offset)
                        g = growth[number]
                        row[element.offset:element.offset + 8] = encode_element(element.type, (dx * g, dy * g, dz * g, w), b"")
                    continue
                second_uv = element.usage == TEXCOORD and element.usage_index == 1 and element.type == 0x2C2359
                if size is None or (element.usage_index != 0 and not second_uv):
                    continue
                at = element.offset
                base = bytes(row[at:at + size])
                values = None
                if element.usage == POSITION:
                    w = struct.unpack(">4h", base)[3] if element.type == 0x1A235A else 0
                    values = tuple(c / position_scale for c in vertex.position) + (w,)
                elif element.usage in (NORMAL, TANGENT, BINORMAL) and unchanged[number]:
                    pass
                elif element.usage == NORMAL and vertex.normal:
                    values = vertex.normal
                elif element.usage == TANGENT and tangents and tangents[number]:
                    values = tangents[number]
                elif element.usage == BINORMAL and binormals and binormals[number]:
                    values = binormals[number]
                elif second_uv:
                    if vertex.second_uv:
                        values = (vertex.second_uv[0] / uv_scale, vertex.second_uv[1] / uv_scale)
                elif element.usage == TEXCOORD and vertex.uv:
                    rest = struct.unpack(">4h", base)[2:] if element.type == 0x1A235A else ()
                    values = (vertex.uv[0] / uv_scale, vertex.uv[1] / uv_scale) + rest
                elif element.usage == COLOR and vertex.colour:
                    # Blender keeps colours as sRGB bytes, so a colour it hands back can be a step off the stored one.
                    r, g, b, a = vertex.colour
                    if any(abs(x - y) > 2 / 255 for x, y in zip((a, r, g, b), decode_element(base, 0, element.type))):
                        values = (a, r, g, b)
                elif element.usage == BLENDINDICES and skin:
                    values = tuple(3 * (skin[0][0] if rigid else slot) for slot in skin[0])
                elif element.usage == BLENDWEIGHT and skin:
                    values = tuple(skin[1][3 - k] / 255.0 for k in range(4))
                if values is not None:
                    row[at:at + size] = encode_element(element.type, values, base)
            out += row
        edits.u32(stream.record, 16, len(out))
        new_blocks[(stream.record.entry.index, stream.record.offset + 12)] = bytes(out)

    for d, draw in enumerate(mesh.draws):
        # A draw with nothing left draws one degenerate triangle.
        strip = _strip([tuple(draw_indices[d][i:i + 3]) for i in range(0, len(draw_indices[d]), 3)]) or [0, 0, 0]
        push = draw.push.ref
        # The push buffer's vertex count is one past the highest index the draw uses.
        edits.u32(resolver.pointer(push, 56), 4 * draw.slot, max(strip) + 1)
        edits.u32(resolver.pointer(push, 64), 4 * draw.slot, PRIMITIVE_TRIANGLE_STRIP)
        edits.u32(resolver.pointer(push, 68), 4 * draw.slot, len(strip) - 2)
        array = resolver.pointer(push, 60)
        new_blocks[(array.entry.index, array.offset + 4 * draw.slot)] = struct.pack(f">{len(strip)}H", *strip)
        header = resolver.pointer(resolver.pointer(push, 52), 4 * draw.slot)
        records = baked.get((header.entry.index, header.offset), [])
        if not records:
            raise ImportError_(f"{name} draw {d}: no draw command in the model's command streams names its index buffer")
        for count in records:
            edits.u32(count, 0, len(strip))


def _growth(mesh: Mesh, vertices: list[tuple], draw_indices: list[list[int]]) -> list[float]:
    """Per new vertex, how much the mesh around it grew: the mean length of its edges over that of the old vertex it
    starts from, 1 where either has none. A blend shape's offsets are scaled by it, so a face made bigger still blinks
    shut."""
    def mean_edges(positions, triangles, count) -> list[float]:
        total, edges = [0.0] * count, [0] * count
        for t in triangles:
            for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                length = math.dist(positions[a], positions[b])
                for v in (a, b):
                    total[v] += length
                    edges[v] += 1
        return [total[v] / edges[v] if edges[v] else 0.0 for v in range(count)]

    old_positions = mesh.positions
    old = mean_edges(old_positions, [t for draw in mesh.draws for t in draw.triangles() if max(t) < len(old_positions)], len(old_positions))
    new_positions = [v[0].position for v in vertices]
    new = mean_edges(new_positions, [tuple(ix[i:i + 3]) for ix in draw_indices for i in range(0, len(ix), 3)], len(vertices))
    out = []
    for number, (_, _, before, _) in enumerate(vertices):
        ratio = new[number] / old[before] if new[number] and old[before] else 1.0
        out.append(1.0 if abs(ratio - 1.0) < 0.02 else ratio)
    return out


def _game_textures(resolver: Resolver) -> dict[tuple[int, int, int], dict[str, Ref]]:
    """Per draw, the game textures its glTF material shows: "colour" its first colour sampler and "normal" its DXN one,
    chosen as the export chooses them."""
    out: dict[tuple[int, int, int], dict[str, Ref]] = {}
    for graph, entry in enumerate(e for e in resolver.archive.entries if resolver.type_name(e) == "rendergraph"):
        palette = texture_palette(resolver, Ref(entry, 0))
        for number, mesh in enumerate(collect_meshes(resolver, rendergraph_roots(resolver, entry))):
            for d, draw in enumerate(mesh.draws):
                if draw.material is None:
                    continue
                slots: dict[str, Ref] = {}
                for sampler in read_material(resolver, draw.material).samplers:
                    if sampler.dynamic or sampler.lightmap or sampler.palette_index >= len(palette) or palette[sampler.palette_index] is None:
                        continue
                    ref = palette[sampler.palette_index]
                    if read_texture(resolver, ref).format_name == "DXN":
                        slots.setdefault("normal", ref)
                    elif sampler.channel == "colour":
                        slots.setdefault("colour", ref)
                out[(graph, number, d)] = slots
    return out


def _import_textures(resolver: Resolver, edits: Edits, gltf: Gltf, notes: list[str], key_of: dict, extra_textures: dict) -> None:
    """Writes the images the glTF's materials show over the game textures they came from, where they differ.

    A texture keeps its format; it may change size (its levels are made again from the image), and the data entry
    holding it is laid out again around it. An image that still matches its texture, within a few levels, leaves it
    untouched. A colour image with no transparency keeps the old texture's alpha, since an export that drops the
    alpha channel would otherwise fill a cut-out texture in. Where several materials show one texture, the first
    decides it."""
    archive = resolver.archive
    targets = _game_textures(resolver)
    for key, slots in extra_textures.items():
        targets[key] = {**targets.get(key, {}), **slots}
    images: dict[tuple[int, int], tuple[str, int, str]] = {}  # texture asset -> (slot, image, material name)
    for material in gltf.doc.get("materials", []):
        match = DRAW_NAME.search(material.get("name", ""))
        if not match:
            continue
        if draw_key(match) not in key_of:
            continue
        for slot, ref in targets.get(key_of[draw_key(match)], {}).items():
            image = gltf.material_image(material, slot)
            if image is not None:
                images.setdefault((ref.entry.index, ref.offset), (slot, image, material["name"]))

    changed: dict[tuple[int, int], tuple] = {}  # texture data (entry, offset) -> (pixels, texture)
    for (entry_index, offset), (slot, image, name) in images.items():
        texture = read_texture(resolver, Ref(archive.entries[entry_index - 1], offset))
        if texture.data is None or texture.kind != 0:
            continue
        old = to_pixels(resolver, texture)
        new = gltf.image(image)
        channels = 2 if slot == "normal" else 3
        same_colour = new.shape == old.shape and int(np.abs(new[:, :, :channels].astype(np.int16) - old[:, :, :channels]).max()) <= 3
        same_alpha = slot == "normal" or (new[:, :, 3] == 255).all() or (new.shape == old.shape and int(np.abs(new[:, :, 3].astype(np.int16) - old[:, :, 3]).max()) <= 3)
        if same_colour and same_alpha:
            continue
        if min(new.shape[:2]) < 32:
            # A side under 32 puts the whole texture in the packed mip tail, which is not written; repeating each texel
            # draws the same image from an ordinary level.
            factor = -(-32 // min(new.shape[:2]))
            new = np.repeat(np.repeat(new, factor, axis=0), factor, axis=1)
        if new.shape[0] % 4 or new.shape[1] % 4:
            raise ImportError_(f"{name}: its {slot} image is {new.shape[1]}x{new.shape[0]}; a texture's sides must be multiples of 4")
        if slot == "colour" and (new[:, :, 3] == 255).all() and (old[:, :, 3] != 255).any():
            from .encode import resize

            new = new.copy()
            new[:, :, 3] = resize(old, new.shape[1], new.shape[0])[:, :, 3]
        changed[(texture.data.entry.index, texture.data.offset)] = (new, texture)
        notes.append(f"{name}: {slot} texture replaced, {old.shape[1]}x{old.shape[0]} -> {new.shape[1]}x{new.shape[0]} {texture.format_name}")
    if not changed:
        return

    # Each data entry holding a replaced texture, laid out again on 4 KB pages; assets sharing data share the new data.
    for data_entry in {key[0] for key in changed}:
        fields = [(r.source, o) for r in archive.relocations if r.target == data_entry for o in r.offsets]
        if any(o != 40 or resolver.type_name(archive.entries[source - 1]) != "texture" for source, o in fields):
            raise ImportError_("a texture's data entry holds other data too; replacing its textures is not supported")
        data = resolver.data(archive.entries[data_entry - 1])
        start = {f: struct.unpack_from(">I", resolver.data(archive.entries[f[0] - 1]), f[1])[0] for f in fields}
        offsets = sorted(set(start.values()))
        out = bytearray()
        moved: dict[int, int] = {}
        for i, old_offset in enumerate(offsets):
            out += bytes(-len(out) % 4096)
            moved[old_offset] = len(out)
            if (data_entry, old_offset) in changed:
                pixels, texture = changed[(data_entry, old_offset)]
                levels = max(pixels.shape[0], pixels.shape[1]).bit_length()
                out += build_texture(pixels, texture, levels)
                for (asset, _), value in start.items():
                    if value == old_offset:
                        struct.pack_into(">HH", edits.entry(asset), 36, pixels.shape[1], pixels.shape[0])
                        edits.entry(asset)[44] = levels
            else:
                end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
                out += data[old_offset:end]
        out += bytes(-len(out) % 4096)
        for (asset, field_offset), value in start.items():
            struct.pack_into(">I", edits.entry(asset), field_offset, moved[value])
        edits.data[data_entry] = out


def _baked_draws(resolver: Resolver) -> dict[tuple[int, int], list[Ref]]:
    """Where the command streams (the .stream section's KameoShaderParamsPostCommandStream) keep each draw's index count.

    The game draws a model from these streams, not from its ndPushBuffer fields: every draw is a packet naming the
    index buffer's header, with the primitive type and index count baked in. The packet word, 0x0014TTxx, sits 16 bytes
    before the header pointer and the primitive type 12 before; a type 0x01 packet keeps the index count just before the
    pointer, a type 0x30 one 8 before (a 0/1 word follows it). Rebuilt index buffers leave these counts stale, and the
    game then draws only the old number of indices of the new strip."""
    archive = resolver.archive
    out: dict[tuple[int, int], list[Ref]] = {}
    for entry in archive.entries:
        if archive.sections[entry.section - 1].name != ".stream":
            continue
        data = resolver.data(entry)
        for offset, (target, value) in archive.pointers_from(entry).items():
            if offset < 16:
                continue
            packet = struct.unpack_from(">I", data, offset - 16)[0]
            kind = (packet >> 8) & 0xFF
            if packet >> 16 != 0x14 or kind not in (0x01, 0x30):
                continue
            out.setdefault((target.index, value), []).append(Ref(entry, offset - (4 if kind == 0x01 else 8)))
    return out


def _strip(triangles: list[tuple[int, int, int]]) -> list[int]:
    """Triangles as one strip, each keeping its first corner first, as the game's own strips decode.

    The strip's window at position i is (s[i], s[i+1], s[i+2]), read as that triangle when i is even and as
    (s[i], s[i+2], s[i+1]) when odd; either way s[i] is the triangle's first corner, so an unedited model's triangles
    come back exactly as they were.

    A triangle (p, q, r) whose first two corners end the strip in the order its window needs takes one more index;
    any other is joined on as last, p, p and then q, r or r, q by the parity it lands on, every window over the join
    repeating an index and so drawing nothing."""
    strip: list[int] = []
    for p, q, r in triangles:
        if not strip:
            strip += [p, q, r]
            continue
        position = len(strip) - 2  # the window one more index would make
        if position % 2 == 0 and (strip[-2], strip[-1]) == (p, q):
            strip.append(r)
        elif position % 2 == 1 and (strip[-2], strip[-1]) == (p, r):
            strip.append(q)
        else:
            strip += [strip[-1], p, p]
            strip += [q, r] if (len(strip) - 1) % 2 == 0 else [r, q]
    return strip


def _relayout(resolver: Resolver, edits: Edits, new_blocks: dict[tuple[int, int], bytes]) -> dict[int, bytes]:
    """Each .gpu entry holding a rebuilt buffer, laid out again with the old buffers that stay and the new ones."""
    archive = resolver.archive
    sizes = _old_block_sizes(resolver)
    gpu_entries = {resolver.pointer(Ref(archive.entries[entry - 1], offset)).entry.index for entry, offset in new_blocks}
    for gpu in gpu_entries:
        fields = [(r.source, offset) for r in archive.relocations if r.target == gpu for offset in r.offsets]
        unknown = [f for f in fields if f not in sizes]
        if unknown:
            raise ImportError_(f"{len(unknown)} pointers into the buffer data are not vertex or index buffers; refusing to move it")
        data = resolver.data(archive.entries[gpu - 1])
        old_value = {f: struct.unpack_from(">I", resolver.data(archive.entries[f[0] - 1]), f[1])[0] for f in fields}
        out = bytearray()
        placed: dict[tuple, int] = {}
        for f in sorted(fields, key=lambda f: old_value[f]):
            key = ("new",) + f if f in new_blocks else ("old", old_value[f])
            if key not in placed:
                out += bytes(-len(out) % BUFFER_ALIGNMENT)
                placed[key] = len(out)
                out += new_blocks[f] if f in new_blocks else data[old_value[f]:old_value[f] + sizes[f]]
            struct.pack_into(">I", edits.entry(f[0]), f[1], placed[key])
        out += bytes(-len(out) % BUFFER_ALIGNMENT)
        edits.data[gpu] = out
    return {index: bytes(data) for index, data in edits.data.items()}


def _old_block_sizes(resolver: Resolver) -> dict[tuple[int, int], int]:
    """Every vertex and index buffer pointer field in the archive, with the size of the data it points at."""
    sizes = {}
    for entry in resolver.archive.entries:
        if resolver.type_name(entry) != "rendergraph":
            continue
        for mesh in collect_meshes(resolver, rendergraph_roots(resolver, entry)):
            table = resolver.pointer(mesh.node.ref, 48)
            for i in range(resolver.u32(mesh.node.ref, 56)):
                record = Ref(table.entry, table.offset + 20 * i)
                sizes[(record.entry.index, record.offset + 12)] = resolver.u32(record, 16)
            for draw in mesh.draws:
                push = draw.push.ref
                array = resolver.pointer(push, 60)
                primitive = resolver.u32(resolver.pointer(push, 64), 4 * draw.slot)
                sizes[(array.entry.index, array.offset + 4 * draw.slot)] = _index_bytes(primitive, resolver.u32(resolver.pointer(push, 68), 4 * draw.slot))
    return sizes
