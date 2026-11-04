"""Reader for Kameo collision blocks: the "hits" entries of .lvl and .mdl CAFF archives (version 04.05.05.0032).

A kameolevel asset points at its room block (entry +52, the asset's +20) and every kameomodel at its own
(entry +56, the asset's +24). The game walks a block with hitHitNode, sizes its per-instance memory with
HitNodeGetInstSize, fills it with xfixgem_hitNode_initRootCollisionInstance, and tests triangles in hitKdtreeLine /
hitKdtreeSphere through the kd-tree iterators (kdTreeStaticEllipsoidIterate and friends). Values are big-endian; every pointer is an offset
into the same entry, relocated by the CAFF loader.

Block header:
  0x00  "hits\0" + "04.05.05.0032\0" (pad to 0x14)
  0x14  root node pointer
  0x18  material table pointer: one u32 of surface flags per material, `material_stride` bytes apart
  0x1C  u16 material stride (4), 0x1E u16 material count (non-zero makes the game turn a hit's material index
        into a pointer into the table; 0 only in blocks with no triangles)
  0x20  f32 joint scale (1.0), 0x24 symbol table pointer (switch-node names), 0x28 u32 (0)

Hit node, 24 bytes plus a per-type tail:
  +0 parent  +4 first child  +8 previous sibling  +12 next sibling  +16 type  +20 offset into the instance memory
  (0 in every shipped block). Types found in data:
  0  group         no tail
  6  kd-tree mesh  +24 mesh count, +28 meshes pointer, +32 loose flag (always 0); the 76-byte meshes follow
  7  transform     176 bytes: +24 name, +28 Maya path, +32 matrix, +96 matrix (the one used), +160 16 bytes
  9  joint         224 bytes: as 7, plus u16 joint index at +218 (the runtime uses that root joint instead)
  11 root          +24 transform-node count, +28 joint count, +32 transform scratch (176 bytes each),
                   +36 joint table (pointers to 224-byte joints)
  12 skinned mesh  +24 clusters, +28 vertices, +32/+36/+40 position/normal/extra bytes, +44 flags, +48 clusters
  13 switch        a named part: the instance word at +20 decides whether its children collide
Matrices are row-vector 4x4 (translation in the last row).

Kd-tree mesh, 76 bytes:
  +0 vertices pointer   +4 u8 vertex format (3 = s16 fixed point, else f32; always 4)   +5 u8 fixed-point shift
  +6 u16 extra bytes per vertex (after the position)   +8 u16 normal bytes   +10 u16 position bytes
  +12 u32 flags (bit 0: per-vertex normals follow the extra bytes; always 0)
  +16 triangles pointer (8 bytes each: u16 v0, v1, v2, material)   +20 triangle count   +24 kd-tree root
  +28 bounds min, +40 bounds max (the box the kd-tree splits), +52 / +64 a second min / max (always equal)
The stride is extra + normal + position bytes (16 in every block). The extra bytes are two s16 shadow-map UVs
(/32767, GetShadow); there is no vertex count, so it is the largest index used plus one. Front faces have
(v1 - v0) x (v2 - v0) as their normal.

Kd-tree node (non-loose), 8 bytes:
  inner: u16 flags (bit 0 clear, axis = (flags >> 1) & 3), u16 split (fraction of the node's box along that axis,
         /65535), u32 pointer to the upper child; the lower child follows at +8.
  leaf:  u16 flags (bit 0 set), u16 triangle count, u32 pointer to that many u16 triangle indices.
Loose trees (node +32 set) use 12-byte inner nodes with four u8 bounds at +8 (/255); none ship, and they are
parsed for their leaves only.

Usage: python -m kameo_modkit.hits <file.lvl|.mdl> [out_dir]
"""

from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass, field

import numpy as np

from .caff import Caff, Entry

MAGIC = b"hits\0"
VERSION = b"04.05.05.0032"

NODE_TYPES = {
    0: "group", 1: "sphere", 3: "line", 4: "triangle-list", 6: "kdtree-mesh", 7: "transform", 8: "ellipsoid",
    9: "joint", 10: "transform2", 11: "root", 12: "skinned-mesh", 13: "switch", 14: "offset-sphere", 15: "terrain",
}
NODE_TAIL = {0: 0, 6: 12, 7: 152, 9: 200, 10: 152, 11: 16, 12: 28, 13: 0}
MESH_SIZE = 76
FIXED_POINT = 3


@dataclass
class KdNode:
    offset: int
    axis: int = -1          # -1 for a leaf
    split: float = 0.0      # world coordinate of the split plane
    lower: "KdNode | None" = None
    upper: "KdNode | None" = None
    triangles: list[int] = field(default_factory=list)
    box: tuple[np.ndarray, np.ndarray] | None = None

    @property
    def leaf(self) -> bool:
        return self.axis < 0

    def leaves(self):
        if self.leaf:
            yield self
            return
        for child in (self.lower, self.upper):
            if child is not None:
                yield from child.leaves()


@dataclass
class KdMesh:
    offset: int
    vertex_format: int
    fixed_shift: int
    extra_size: int
    normal_size: int
    position_size: int
    flags: int
    triangles: np.ndarray     # (n, 4) uint16: v0, v1, v2, material
    vertices: np.ndarray      # (m, 3) float64
    extras: np.ndarray        # (m, 2) int16 shadow-map UVs, empty when extra_size < 4
    bounds: tuple[np.ndarray, np.ndarray]
    bounds2: tuple[np.ndarray, np.ndarray]
    tree: KdNode | None
    vertices_offset: int
    triangles_offset: int
    tree_offset: int

    @property
    def stride(self) -> int:
        return self.extra_size + self.normal_size + self.position_size


@dataclass
class HitNode:
    offset: int
    type: int
    instance_offset: int
    parent: int | None
    previous: int | None
    raw_tail: bytes = b""
    children: list["HitNode"] = field(default_factory=list)
    meshes: list[KdMesh] = field(default_factory=list)
    loose: bool = False
    transform_count: int = 0
    joint_count: int = 0
    joint_index: int = -1
    labels: tuple[str, str] = ("", "")
    local_matrix: np.ndarray | None = None   # +32, 4x4 (types 7, 9, 10)
    matrix: np.ndarray | None = None         # +96, 4x4; the one GetStackTransform concatenates
    name: str | None = None

    @property
    def type_name(self) -> str:
        return NODE_TYPES.get(self.type, f"type{self.type}")

    def walk(self, depth: int = 0):
        yield depth, self
        for child in self.children:
            yield from child.walk(depth + 1)


@dataclass
class Hits:
    root: HitNode
    materials: list[int]
    material_stride: int
    joint_scale: float
    unknown_28: int
    parts: dict[str, int]                     # part name -> switch node offset
    size: int

    def nodes(self):
        yield from (node for _, node in self.root.walk())

    def meshes(self):
        for node in self.nodes():
            yield from node.meshes

    def triangle_count(self) -> int:
        return sum(len(m.triangles) for m in self.meshes())


class _Block:
    def __init__(self, data: bytes, pointers: set[int]):
        self.data = data
        self.pointers = pointers

    def u8(self, o: int) -> int:
        return self.data[o]

    def u16(self, o: int) -> int:
        return struct.unpack_from(">H", self.data, o)[0]

    def u32(self, o: int) -> int:
        return struct.unpack_from(">I", self.data, o)[0]

    def f32(self, o: int) -> float:
        return struct.unpack_from(">f", self.data, o)[0]

    def vec(self, o: int) -> np.ndarray:
        return np.array(struct.unpack_from(">3f", self.data, o), dtype=np.float64)

    def string(self, o: int | None) -> str:
        if o is None:
            return ""
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")

    def ptr(self, o: int) -> int | None:
        """The block offset a pointer field holds, or None when the field is not relocated (a null pointer)."""
        return self.u32(o) if o in self.pointers else None


def parse(data: bytes, pointers: set[int] | None = None) -> Hits:
    """Parse a hits block. `pointers` is the set of relocated field offsets; without it, non-zero means pointer."""
    if data[:5] != MAGIC or data[5:5 + len(VERSION)] != VERSION:
        raise ValueError("not a hits 04.05.05.0032 block")
    if pointers is None:
        pointers = {o for o in range(0, len(data) - 3, 4) if struct.unpack_from(">I", data, o)[0]}
    b = _Block(data, pointers)
    materials_at = b.ptr(0x18)
    stride = b.u16(0x1C)
    count = b.u16(0x1E)
    materials = [b.u32(materials_at + stride * i) for i in range(count)] if materials_at is not None else []
    parts = _symbols(b, b.ptr(0x24))
    root = _chain(b, b.ptr(0x14), None, set())[0]
    names = {offset: name for name, offset in parts.items()}
    for _, node in root.walk():
        node.name = names.get(node.offset)
    return Hits(root, materials, stride, b.f32(0x20), b.u32(0x28), parts, len(data))


def _symbols(b: _Block, at: int | None) -> dict[str, int]:
    """Rare's symbol table: count, then a {name, name, value} array (the hash table at +12 indexes it)."""
    if at is None:
        return {}
    found = {}
    array = b.ptr(at + 4)
    for i in range(b.u32(at)):
        entry = array + 12 * i
        name_at = b.ptr(entry)
        end = b.data.index(b"\0", name_at)
        found[b.data[name_at:end].decode("latin-1")] = b.ptr(entry + 8)
    return found


def _chain(b: _Block, at: int | None, parent: int | None, seen: set[int]) -> list[HitNode]:
    """A node and its next siblings, each with its children."""
    chain: list[HitNode] = []
    previous = None
    while at is not None:
        if at in seen:
            raise ValueError(f"hit node loop at {at:#x}")
        seen.add(at)
        kind = b.u32(at + 16)
        tail = NODE_TAIL.get(kind)
        if tail is None:
            raise ValueError(f"unsupported hit node type {kind} at {at:#x}")
        node = HitNode(at, kind, b.u32(at + 20), parent, previous, b.data[at + 24:at + 24 + tail])
        if kind == 6:
            node.loose = bool(b.u32(at + 32))
            records = b.ptr(at + 28)
            node.meshes = [_mesh(b, records + MESH_SIZE * i, node.loose) for i in range(b.u32(at + 24))]
        elif kind == 11:
            node.transform_count = b.u32(at + 24)
            node.joint_count = b.u32(at + 28)
        elif kind == 12:
            node.meshes = [_skinned(b, at)]
        elif kind in (7, 9, 10):
            node.labels = (b.string(b.ptr(at + 24)), b.string(b.ptr(at + 28)))
            node.local_matrix = np.array(struct.unpack_from(">16f", b.data, at + 32), dtype=np.float64).reshape(4, 4)
            node.matrix = np.array(struct.unpack_from(">16f", b.data, at + 96), dtype=np.float64).reshape(4, 4)
            if kind == 9:
                node.joint_index = b.u16(at + 218)
        node.children = _chain(b, b.ptr(at + 4), at, seen)
        chain.append(node)
        previous = at
        at = b.ptr(at + 12)
    return chain


def _mesh(b: _Block, at: int, loose: bool) -> KdMesh:
    vertices_at = b.ptr(at)
    vertex_format, shift = b.u8(at + 4), b.u8(at + 5)
    extra, normal, position = b.u16(at + 6), b.u16(at + 8), b.u16(at + 10)
    flags = b.u32(at + 12)
    tris_at, tri_count, tree_at = b.ptr(at + 16), b.u32(at + 20), b.ptr(at + 24)
    triangles = np.frombuffer(b.data, dtype=">u2", count=4 * tri_count, offset=tris_at).reshape(-1, 4).astype(np.uint16)
    vertex_count = int(triangles[:, :3].max()) + 1 if tri_count else 0
    stride = extra + normal + position
    raw = np.frombuffer(b.data, dtype=np.uint8, count=stride * vertex_count, offset=vertices_at).reshape(-1, stride)
    if vertex_format == FIXED_POINT:
        vertices = raw[:, :6].copy().view(">i2").astype(np.float64) / float(1 << shift)
    else:
        vertices = raw[:, :12].copy().view(">f4").astype(np.float64)
    extras = raw[:, position:position + 4].copy().view(">i2").astype(np.int16) if extra >= 4 else np.zeros((0, 2), np.int16)
    bounds = (b.vec(at + 28), b.vec(at + 40))
    bounds2 = (b.vec(at + 52), b.vec(at + 64))
    tree = _kd_loose(b, tree_at) if loose else _kd(b, tree_at, bounds[0].copy(), bounds[1].copy())
    return KdMesh(at, vertex_format, shift, extra, normal, position, flags, triangles, vertices, extras, bounds,
                  bounds2, tree, vertices_at, tris_at, tree_at)


def _skinned(b: _Block, at: int) -> KdMesh:
    """A skinned mesh (type 12): +24 clusters (16 bytes: u32 joint, f32 radius, triangles pointer, count),
    +28 vertices, +32/+36/+40 position/normal/extra bytes, +44 flags, +48 cluster count. Its triangles are
    92 bytes: u16 v0, v1, v2, material, then three 28-byte skin bindings (u16 joints[4], f32 weights[4],
    u32 count) that HitSkinned applies per corner. Returned as a KdMesh without a tree, in the bind pose."""
    clusters_at, vertices_at = b.ptr(at + 24), b.ptr(at + 28)
    position, normal, extra = b.u32(at + 32), b.u32(at + 36), b.u32(at + 40)
    rows = []
    for i in range(b.u32(at + 48)):
        cluster = clusters_at + 16 * i
        tris = b.ptr(cluster + 8)
        for k in range(b.u32(cluster + 12)):
            rows.append(struct.unpack_from(">4H", b.data, tris + 92 * k))
    triangles = np.array(rows, dtype=np.uint16).reshape(-1, 4)
    stride = position + normal + extra
    count = int(triangles[:, :3].max()) + 1 if len(triangles) else 0
    raw = np.frombuffer(b.data, dtype=np.uint8, count=stride * count, offset=vertices_at).reshape(-1, stride)
    vertices = raw[:, :12].copy().view(">f4").astype(np.float64)
    extras = raw[:, position:position + 4].copy().view(">i2").astype(np.int16) if extra >= 4 else np.zeros((0, 2), np.int16)
    low, high = (vertices.min(axis=0), vertices.max(axis=0)) if count else (np.zeros(3), np.zeros(3))
    return KdMesh(at, 4, 0, extra, normal, position, b.u32(at + 44), triangles, vertices, extras, (low, high),
                  (low, high), None, vertices_at, clusters_at, 0)


def _leaf(b: _Block, at: int, box) -> KdNode:
    count = b.u16(at + 2)
    list_at = b.ptr(at + 4)
    triangles = list(struct.unpack_from(f">{count}H", b.data, list_at)) if count and list_at is not None else []
    return KdNode(at, triangles=triangles, box=box)


def _kd(b: _Block, at: int | None, low: np.ndarray, high: np.ndarray) -> KdNode | None:
    if at is None:
        return None
    flags = b.u16(at)
    if flags & 1:
        return _leaf(b, at, (low, high))
    axis = (flags >> 1) & 3
    split = low[axis] + b.u16(at + 2) / 65535.0 * (high[axis] - low[axis])
    lower_high = high.copy()
    lower_high[axis] = split
    upper_low = low.copy()
    upper_low[axis] = split
    node = KdNode(at, axis, split, box=(low, high))
    node.lower = _kd(b, at + 8, low.copy(), lower_high)
    node.upper = _kd(b, b.ptr(at + 4), upper_low, high.copy())
    return node


def _kd_loose(b: _Block, at: int | None) -> KdNode | None:
    if at is None:
        return None
    flags = b.u16(at)
    if flags & 1:
        return _leaf(b, at, None)
    node = KdNode(at, (flags >> 1) & 3)
    node.lower = _kd_loose(b, at + 12)
    node.upper = _kd_loose(b, b.ptr(at + 4))
    return node


def load_entry(archive: Caff, entry: Entry) -> Hits:
    data = entry.read(archive)
    pointers = set()
    for relocation in archive.relocations:
        if relocation.source != entry.index:
            continue
        if relocation.target != entry.index:
            raise ValueError(f"hits entry {entry.index} points into entry {relocation.target}")
        pointers.update(relocation.offsets)
    return parse(data, pointers)


def hits_entries(archive: Caff) -> list[Entry]:
    return [e for e in archive.entries if e.read(archive)[:5] == MAGIC]


def check(hits: Hits) -> list[str]:
    """Consistency checks a writer has to satisfy: every triangle in some leaf, and each leaf's triangles touching
    the box the kd-tree gives it. A leaf can hang under both sides of a split (the builder points the upper child
    at the lower leaf when a split separated nothing), so a shared leaf is checked against the union of its boxes."""
    problems = []
    for mesh in hits.meshes():
        n = len(mesh.triangles)
        if mesh.tree is None:
            continue   # skinned meshes have clusters instead
        seen = np.zeros(n, dtype=bool)
        boxes: dict[int, tuple] = {}
        for leaf in mesh.tree.leaves():
            if leaf.box is None:
                boxes.setdefault(leaf.offset, (None, None, leaf))
            elif leaf.offset in boxes and boxes[leaf.offset][0] is not None:
                low, high, _ = boxes[leaf.offset]
                boxes[leaf.offset] = (np.minimum(low, leaf.box[0]), np.maximum(high, leaf.box[1]), leaf)
            else:
                boxes[leaf.offset] = (leaf.box[0].copy(), leaf.box[1].copy(), leaf)
        extent = float(np.abs(mesh.bounds[1] - mesh.bounds[0]).max())
        # Splits are 16-bit fractions of the parent box and the builder's own boxes drift from the decoded ones by
        # up to a step per level, so allow a few dozen root-box steps.
        slack = 64.0 * max(extent, 1.0) / 65535.0
        for box_low, box_high, leaf in boxes.values():
            if any(t >= n for t in leaf.triangles):
                problems.append(f"mesh {mesh.offset:#x}: leaf {leaf.offset:#x} indexes past the triangle array")
                continue
            seen[leaf.triangles] = True
            if box_low is not None and leaf.triangles:
                corners = mesh.vertices[mesh.triangles[leaf.triangles, :3].astype(int)]
                low, high = corners.min(axis=1), corners.max(axis=1)
                outside = np.any(high < box_low - slack, axis=1) | np.any(low > box_high + slack, axis=1)
                if outside.any():
                    problems.append(f"mesh {mesh.offset:#x}: leaf {leaf.offset:#x} has {int(outside.sum())} triangles outside its box")
        if not seen.all():
            problems.append(f"mesh {mesh.offset:#x}: {int((~seen).sum())} triangles in no leaf")
        if n:
            low, high = mesh.vertices.min(axis=0), mesh.vertices.max(axis=0)
            if np.any(low < mesh.bounds[0] - slack) or np.any(high > mesh.bounds[1] + slack):
                problems.append(f"mesh {mesh.offset:#x}: vertices outside the stored bounds")
            if int(mesh.triangles[:, 3].max()) >= max(len(hits.materials), 1):
                problems.append(f"mesh {mesh.offset:#x}: material index past the table ({len(hits.materials)} entries)")
    return problems


def node_transforms(hits: Hits) -> dict[int, np.ndarray]:
    """Rest-pose model-space matrix of every node: the +96 matrices of the transform and joint nodes above it,
    row vectors (translation in the last row), innermost first. At run time a joint node uses its joint table
    entry instead, and the whole block is drawn with the object's own matrix."""
    result: dict[int, np.ndarray] = {}

    def visit(node: HitNode, above: np.ndarray):
        here = node.matrix @ above if node.matrix is not None else above
        result[node.offset] = here
        for child in node.children:
            visit(child, here)

    visit(hits.root, np.eye(4))
    return result


def write_obj(hits: Hits, out, name: str = "hits", transforms: bool = True) -> int:
    """Write every collision triangle as OBJ in game coordinates (no axis flip), one group per mesh and one
    material per surface-flag value. Skinned meshes are written in their bind pose. Returns the triangle count."""
    out.write(f"# Kameo collision ({name}): {hits.triangle_count()} triangles, materials {[hex(m) for m in hits.materials]}\n")
    matrices = node_transforms(hits) if transforms else {}
    base = 1
    written = 0
    for node in hits.nodes():
        for number, mesh in enumerate(node.meshes):
            label = node.name or f"node{node.offset:x}"
            out.write(f"g {label}_{node.type_name}{number}\n")
            vertices = mesh.vertices
            matrix = matrices.get(node.offset)
            if matrix is not None and not np.allclose(matrix, np.eye(4)):
                vertices = (np.c_[vertices, np.ones(len(vertices))] @ matrix)[:, :3]
            for x, y, z in vertices:
                out.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
            current = None
            for v0, v1, v2, material in mesh.triangles:
                flags = hits.materials[material] if material < len(hits.materials) else None
                key = f"surface_{flags:08x}" if flags is not None else f"material_{material}"
                if key != current:
                    out.write(f"usemtl {key}\n")
                    current = key
                out.write(f"f {v0 + base} {v1 + base} {v2 + base}\n")
                written += 1
            base += len(mesh.vertices)
    return written


def describe(hits: Hits) -> str:
    lines = [f"hits block, {hits.size} bytes, materials {[hex(m) for m in hits.materials]} (stride {hits.material_stride}), "
             f"joint scale {hits.joint_scale}, parts {sorted(hits.parts)}"]
    for depth, node in hits.root.walk():
        text = f"{'  ' * depth}{node.type_name} @{node.offset:#x} inst+{node.instance_offset}"
        if node.name:
            text += f" '{node.name}'"
        if node.labels[0]:
            text += f" {node.labels[0]!r}" + (f" joint {node.joint_index}" if node.type == 9 else "")
        if node.type == 11:
            text += f" transforms {node.transform_count}, joints {node.joint_count}"
        lines.append(text)
        for mesh in node.meshes:
            leaves = list(mesh.tree.leaves()) if mesh.tree else []
            lines.append(f"{'  ' * depth}  mesh @{mesh.offset:#x}: {len(mesh.triangles)} triangles, {len(mesh.vertices)} vertices, "
                         f"format {mesh.vertex_format} shift {mesh.fixed_shift}, stride {mesh.stride} "
                         f"({mesh.position_size}+{mesh.extra_size}+{mesh.normal_size}), flags {mesh.flags:#x}, "
                         f"{len(leaves)} leaves{' (loose)' if node.loose else ''}, bounds {np.round(mesh.bounds[0], 2)} .. {np.round(mesh.bounds[1], 2)}")
    return "\n".join(lines)


# Writing.
#
# A mesh's payload is its triangles, its kd-tree with the leaves' triangle lists, and its vertices, laid out at an
# offset inside the block so its pointers can be written as block offsets. The tree splits each node's box at a u16
# fraction along its longest axis, near the median triangle centre, and works out the child boxes from the quantised
# fraction exactly as the reader (and the game) does; a triangle goes to every child its box touches. A node becomes a
# leaf when it holds few triangles, gets too deep, or a split would leave a side empty.

def _quantised_split(low: float, high: float, at: float) -> tuple[int, float]:
    fraction = (at - low) / (high - low)
    q = int(round(min(max(fraction, 1.0 / 65535.0), 65534.0 / 65535.0) * 65535.0))
    return q, low + q / 65535.0 * (high - low)


def _build_tree(boxes_low: np.ndarray, boxes_high: np.ndarray, centres: np.ndarray, ids: np.ndarray,
                low: np.ndarray, high: np.ndarray, depth: int, leaf_size: int, max_depth: int):
    """A nested tuple tree: ("leaf", ids) or ("inner", axis, q, lower, upper)."""
    extent = high - low
    if len(ids) <= leaf_size or depth >= max_depth:
        return ("leaf", ids)
    for axis in np.argsort(-extent):
        if extent[axis] < 1e-4:
            break
        q, split = _quantised_split(float(low[axis]), float(high[axis]), float(np.median(centres[ids, axis])))
        epsilon = 1e-4 * max(1.0, float(extent[axis]))
        lower = ids[boxes_low[ids, axis] <= split + epsilon]
        upper = ids[boxes_high[ids, axis] >= split - epsilon]
        if len(lower) == 0 or len(upper) == 0 or (len(lower) == len(ids) and len(upper) == len(ids)):
            continue
        lower_high, upper_low = high.copy(), low.copy()
        lower_high[axis] = split
        upper_low[axis] = split
        return ("inner", int(axis), q,
                _build_tree(boxes_low, boxes_high, centres, lower, low, lower_high, depth + 1, leaf_size, max_depth),
                _build_tree(boxes_low, boxes_high, centres, upper, upper_low, high, depth + 1, leaf_size, max_depth))
    return ("leaf", ids)


def mesh_payload(vertices: np.ndarray, triangles: np.ndarray, base: int, leaf_size: int = 8, max_depth: int = 16,
                 pad: float = 0.01) -> tuple[bytes, bytes, list[int], list[int]]:
    """One kd-tree mesh's data placed at block offset `base` (a multiple of 16).

    `vertices` is (m, 3) in game space and `triangles` (n, 4): v0, v1, v2, material index, front faces with
    (v1 - v0) x (v2 - v0) as their normal. Returns the payload, the 76-byte mesh record pointing into it, the payload's
    pointer fields as block offsets, and the record's pointer fields as offsets inside the record."""
    vertices = np.asarray(vertices, dtype=np.float32).reshape(-1, 3)
    triangles = np.asarray(triangles, dtype=np.int64).reshape(-1, 4)
    if not len(triangles) or len(triangles) > 0xFFFF or len(vertices) > 0x10000:
        raise ValueError(f"a collision mesh holds 1 to 65535 triangles and up to 65536 vertices ({len(triangles)}, {len(vertices)})")
    corners = vertices[triangles[:, :3]]
    boxes_low, boxes_high, centres = corners.min(axis=1), corners.max(axis=1), corners.mean(axis=1)
    # The box as the record stores it (f32), which is what the game splits.
    low = (vertices.min(axis=0) - np.float32(pad)).astype(np.float32).astype(np.float64)
    high = (vertices.max(axis=0) + np.float32(pad)).astype(np.float32).astype(np.float64)
    tree = _build_tree(boxes_low, boxes_high, centres, np.arange(len(triangles)), low, high, 0, leaf_size, max_depth)

    out = bytearray()
    pointers: list[int] = []
    out += b"".join(struct.pack(">4H", *row) for row in triangles)
    triangles_at = base
    out += bytes(-len(out) % 8)
    tree_at = base + len(out)
    leaves: list[tuple[int, np.ndarray]] = []

    def emit(node):
        at = len(out)
        out.extend(bytes(8))
        if node[0] == "leaf":
            struct.pack_into(">HH", out, at, 1, len(node[1]))
            leaves.append((at, node[1]))
            return
        _, axis, q, lower, upper = node
        struct.pack_into(">HH", out, at, axis << 1, q)
        emit(lower)
        struct.pack_into(">I", out, at + 4, base + len(out))
        pointers.append(base + at + 4)
        emit(upper)

    emit(tree)
    for at, ids in leaves:
        struct.pack_into(">I", out, at + 4, base + len(out))
        pointers.append(base + at + 4)
        out += np.asarray(ids, dtype=">u2").tobytes()
    out += bytes(-len(out) % 16)
    vertices_at = base + len(out)
    # Position, then the two s16 shadow-map coordinates, left at 0.
    for x, y, z in vertices:
        out += struct.pack(">3f2h", x, y, z, 0, 0)

    record = bytearray(MESH_SIZE)
    struct.pack_into(">IBBHHHI", record, 0, vertices_at, 4, 0, 4, 0, 12, 0)
    struct.pack_into(">III", record, 16, triangles_at, len(triangles), tree_at)
    struct.pack_into(">3f3f3f3f", record, 28, *low, *high, *low, *high)
    return bytes(out), bytes(record), pointers, [0, 16, 24]


def replace_mesh(data: bytes, pointers: set[int], mesh_at: int, vertices: np.ndarray, triangles: np.ndarray,
                 **tree_options) -> tuple[bytes, list[int]]:
    """The block with the kd-tree mesh record at `mesh_at` given new geometry, appended to the block; its old data is
    left behind unused. Returns the new block and its full pointer list for the entry's self-relocation."""
    block = bytearray(data)
    block += bytes(-len(block) % 16)
    payload, record, payload_pointers, record_pointers = mesh_payload(vertices, triangles, len(block), **tree_options)
    block += payload
    block[mesh_at:mesh_at + MESH_SIZE] = record
    all_pointers = set(pointers) | set(payload_pointers) | {mesh_at + p for p in record_pointers}
    return bytes(block), sorted(all_pointers)


def set_self_pointers(archive: Caff, entry: Entry, pointers: list[int]) -> None:
    """Gives `entry` the self-relocation list `pointers`, so Caff.write relocates the rewritten block."""
    from .caff import Relocation
    for relocation in archive.relocations:
        if relocation.source == entry.index and relocation.target == entry.index:
            relocation.offsets = list(pointers)
            return
    archive.relocations.append(Relocation(entry.index, entry.index, list(pointers)))


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: python -m kameo_modkit.hits <file.lvl|.mdl> [out_dir]   (dumps every hits entry to OBJ)")
        return 1
    archive = Caff.load(argv[1])
    out_dir = argv[2] if len(argv) > 2 else None
    stem = os.path.splitext(os.path.basename(argv[1]))[0]
    for entry in hits_entries(archive):
        hits = load_entry(archive, entry)
        print(f"entry {entry.index} (symbol {entry.symbol})")
        print(describe(hits))
        problems = check(hits)
        for problem in problems[:5]:
            print("  CHECK:", problem)
        if len(problems) > 5:
            print(f"  CHECK: ... {len(problems) - 5} more")
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
            path = os.path.join(out_dir, f"{stem}_hits{entry.index}.obj")
            with open(path, "w") as handle:
                count = write_obj(hits, handle, f"{stem} entry {entry.index}")
            print(f"  wrote {path} ({count} triangles)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
