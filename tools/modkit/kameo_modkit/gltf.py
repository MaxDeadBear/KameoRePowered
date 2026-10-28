"""Exports a model archive to glTF 2.0: meshes with normals, UVs, skin weights, the skeleton and the textures.

Written as `<name>.gltf` with `<name>.bin` and the textures as PNGs in `<name>_textures/`, so the images can be edited
in any paint program and Blender's importer reads the whole model.

Coordinates: the game is left-handed, a model facing +Z, and glTF right-handed with +Z its front, so X is negated
(positions, normals, joint positions) and every triangle's winding reversed. Negating Z would convert the handedness
too, but would turn the model to face away. Units and the Y-up axis carry over as they are.

Skinning (see skeleton.py and mesh.py): every draw has its own palette of up to 12 matrices, and a vertex's
BLENDINDICES are register offsets into it (three registers a matrix). The weights are stored in reverse, index k
taking byte 3 - k; a vertex buffer without BLENDWEIGHT is rigid, its first index carrying the whole weight. Each draw's
vertices are written once per draw with joint numbers resolved through its palette, since two draws over one buffer
can mean different joints by the same index.

Every draw gets its own material, named graph<g>_mesh<m>_draw<d> (draws often share the game's material but each has
its own bone palette), and the import (importer.py) sends triangles back to the draw their material names. Vertex
colours, stored A, R, G, B, go out as COLOR_0, and a second UV set as TEXCOORD_1.
"""

from __future__ import annotations

import json
import math
import os
import struct

from .caff import Caff
from .graph import Ref, Resolver, rendergraph_roots
from .material import read_material, texture_palette
from .mesh import BLENDINDICES, BLENDWEIGHT, COLOR, NORMAL, TEXCOORD, collect_meshes, has_second_uvs
from .skeleton import Skeleton, read_skeleton

FLOAT, UNSIGNED_SHORT, UNSIGNED_INT = 5126, 5123, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963


class _Builder:
    def __init__(self) -> None:
        self.blob = bytearray()
        self.views: list[dict] = []
        self.accessors: list[dict] = []

    def _view(self, data: bytes, target: int | None) -> int:
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.blob += data
        self.views.append(view)
        return len(self.views) - 1

    def floats(self, rows: list[tuple], kind: str, bounds: bool = False) -> int:
        width = len(rows[0])
        data = struct.pack(f"<{len(rows) * width}f", *(c for row in rows for c in row))
        accessor = {"bufferView": self._view(data, ARRAY_BUFFER), "componentType": FLOAT, "count": len(rows), "type": kind}
        if bounds:
            accessor["min"] = [min(row[k] for row in rows) for k in range(width)]
            accessor["max"] = [max(row[k] for row in rows) for k in range(width)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def joints(self, rows: list[tuple[int, int, int, int]]) -> int:
        data = struct.pack(f"<{len(rows) * 4}H", *(c for row in rows for c in row))
        self.accessors.append({"bufferView": self._view(data, ARRAY_BUFFER), "componentType": UNSIGNED_SHORT, "count": len(rows), "type": "VEC4"})
        return len(self.accessors) - 1

    def indices(self, values: list[int]) -> int:
        data = struct.pack(f"<{len(values)}I", *values)
        self.accessors.append({"bufferView": self._view(data, ELEMENT_ARRAY_BUFFER), "componentType": UNSIGNED_INT, "count": len(values), "type": "SCALAR"})
        return len(self.accessors) - 1

    def matrices(self, mats: list[list[float]]) -> int:
        data = struct.pack(f"<{len(mats) * 16}f", *(v for m in mats for v in m))
        self.accessors.append({"bufferView": self._view(data, None), "componentType": FLOAT, "count": len(mats), "type": "MAT4"})
        return len(self.accessors) - 1


def _flip(v: tuple[float, float, float]) -> tuple[float, float, float]:
    return (-v[0], v[1], v[2])


def _skin_of_vertex(index: tuple, weight: tuple | None, palette: list[int], skeleton: Skeleton) -> tuple[tuple[int, int, int, int], tuple[float, float, float, float]]:
    joints = [0, 0, 0, 0]
    weights = [0.0, 0.0, 0.0, 0.0]
    for k in range(4):
        w = (weight[3 - k] if weight is not None else (1.0 if k == 0 else 0.0))
        if w <= 0.0:
            continue
        slot = int(index[k]) // 3
        if slot >= len(palette):
            continue
        joints[k] = skeleton.joint_of_matrix(palette[slot])
        weights[k] = w
    total = sum(weights)
    if total <= 0.0:
        slot = int(index[0]) // 3
        joints[0] = skeleton.joint_of_matrix(palette[slot]) if slot < len(palette) else 0
        weights = [1.0, 0.0, 0.0, 0.0]
    else:
        weights = [w / total for w in weights]
    return tuple(joints), tuple(weights)


def export_gltf(path: str, out: str) -> None:
    from .__main__ import TextureWriter  # the PNG writer the OBJ export uses

    archive = Caff.load(path)
    resolver = Resolver(archive)
    stem = os.path.splitext(os.path.basename(out))[0]
    folder = os.path.dirname(os.path.abspath(out))
    textures = TextureWriter(resolver, os.path.join(folder, f"{stem}_textures"), f"{stem}_textures")
    build = _Builder()
    nodes: list[dict] = []
    meshes: list[dict] = []
    materials: list[dict] = []
    images: list[dict] = []
    gl_textures: list[dict] = []
    skins: list[dict] = []
    scene_nodes: list[int] = []
    image_index: dict[str, int] = {}

    def texture_of(png: str) -> int:
        if png not in image_index:
            images.append({"uri": png.replace(os.sep, "/")})
            gl_textures.append({"source": len(images) - 1})
            image_index[png] = len(gl_textures) - 1
        return image_index[png]

    for graph_number, entry in enumerate(e for e in archive.entries if resolver.type_name(e) == "rendergraph"):
        palette = texture_palette(resolver, Ref(entry, 0))
        roots = rendergraph_roots(resolver, entry)
        skeleton = read_skeleton(resolver, roots)

        skin_index = None
        if skeleton is not None:
            first_joint = len(nodes)
            for joint in skeleton.joints:
                # The rest pose is the bind pose: each joint sits at its pivot, whatever its stored translation says.
                parent_pivot = skeleton.joints[joint.parent].pivot if joint.parent >= 0 else (0.0, 0.0, 0.0)
                local = tuple(joint.pivot[k] - parent_pivot[k] for k in range(3))
                nodes.append({"name": f"joint_{joint.index:02d}", "translation": list(_flip(local))})
            for joint in skeleton.joints:
                children = [first_joint + j.index for j in skeleton.joints if j.parent == joint.index]
                if children:
                    nodes[first_joint + joint.index]["children"] = children
            inverse_binds = []
            for joint in skeleton.joints:
                x, y, z = _flip(joint.pivot)
                inverse_binds.append([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1])
            skins.append({"name": f"graph{graph_number}_skin", "joints": [first_joint + j.index for j in skeleton.joints], "inverseBindMatrices": build.matrices(inverse_binds), "skeleton": first_joint + next(j.index for j in skeleton.joints if j.parent < 0)})
            skin_index = len(skins) - 1
            scene_nodes.extend(first_joint + j.index for j in skeleton.joints if j.parent < 0)

        for mesh_number, mesh in enumerate(collect_meshes(resolver, roots)):
            positions = mesh.positions
            normals = mesh.attributes.get((NORMAL, 0), [])
            uvs = mesh.uvs() if (TEXCOORD, 0) in mesh.attributes else []
            second_uvs = mesh.uvs(1) if has_second_uvs(mesh) else []
            blend_index = mesh.attributes.get((BLENDINDICES, 0))
            blend_weight = mesh.attributes.get((BLENDWEIGHT, 0))
            colours = mesh.attributes.get((COLOR, 0), [])
            primitives = []
            for draw_number, draw in enumerate(mesh.draws):
                triangles = [t for t in draw.triangles() if max(t) < len(positions) and len(set(t)) == 3]
                if not triangles:
                    continue
                remap: dict[int, int] = {}
                order: list[int] = []
                for tri in triangles:
                    for v in tri:
                        if v not in remap:
                            remap[v] = len(order)
                            order.append(v)
                attributes = {"POSITION": build.floats([_flip(positions[v]) for v in order], "VEC3", bounds=True)}
                if normals:
                    rows = []
                    for v in order:
                        n = normals[v][:3] if v < len(normals) else (0.0, 1.0, 0.0)
                        length = math.sqrt(sum(c * c for c in n)) or 1.0
                        rows.append(_flip(tuple(c / length for c in n)) if length > 1e-6 else (0.0, 1.0, 0.0))
                    attributes["NORMAL"] = build.floats(rows, "VEC3")
                if uvs:
                    attributes["TEXCOORD_0"] = build.floats([tuple(uvs[v]) if v < len(uvs) else (0.0, 0.0) for v in order], "VEC2")
                if second_uvs:
                    attributes["TEXCOORD_1"] = build.floats([tuple(second_uvs[v]) if v < len(second_uvs) else (0.0, 0.0) for v in order], "VEC2")
                if colours:
                    attributes["COLOR_0"] = build.floats([(colours[v][1], colours[v][2], colours[v][3], colours[v][0]) if v < len(colours) else (1.0, 1.0, 1.0, 1.0) for v in order], "VEC4")
                if skin_index is not None and blend_index:
                    pairs = [_skin_of_vertex(blend_index[v], blend_weight[v] if blend_weight else None, draw.palette, skeleton) for v in order]
                    attributes["JOINTS_0"] = build.joints([p[0] for p in pairs])
                    attributes["WEIGHTS_0"] = build.floats([p[1] for p in pairs], "VEC4")
                # Reversed winding, for the mirrored X.
                indices = [remap[v] for tri in triangles for v in (tri[0], tri[2], tri[1])]
                primitive = {"attributes": attributes, "indices": build.indices(indices), "mode": 4, "material": len(materials)}
                name = f"graph{graph_number}_mesh{mesh_number}_draw{draw_number}"
                materials.append(_material(resolver, draw.material, name, palette, textures, texture_of) if draw.material is not None else {"name": name})
                primitives.append(primitive)
            if not primitives:
                continue
            meshes.append({"name": f"graph{graph_number}_mesh{mesh_number}", "primitives": primitives})
            node = {"name": f"graph{graph_number}_mesh{mesh_number}", "mesh": len(meshes) - 1}
            if skin_index is not None and any("JOINTS_0" in p["attributes"] for p in primitives):
                node["skin"] = skin_index
            nodes.append(node)
            scene_nodes.append(len(nodes) - 1)

    bin_name = f"{stem}.bin"
    document = {
        "asset": {"version": "2.0", "generator": "kameo_modkit"},
        "scene": 0,
        "scenes": [{"nodes": scene_nodes}],
        "nodes": nodes,
        "meshes": meshes,
        "buffers": [{"uri": bin_name, "byteLength": len(build.blob)}],
        "bufferViews": build.views,
        "accessors": build.accessors,
    }
    if materials:
        document["materials"] = materials
    if images:
        document["images"] = images
        document["textures"] = gl_textures
    if skins:
        document["skins"] = skins
    with open(os.path.join(folder, bin_name), "wb") as handle:
        handle.write(build.blob)
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=1)
    joints = sum(len(s["joints"]) for s in skins)
    print(f"wrote {out}: {len(meshes)} meshes, {sum(len(m['primitives']) for m in meshes)} primitives, {len(materials)} materials, {len(images)} textures, {joints} joints")


def _material(resolver: Resolver, node, name: str, palette: list, textures, texture_of) -> dict:
    material = read_material(resolver, node)
    out: dict = {"name": name, "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 0.8}}
    colour = material.base_colour()
    if colour and not material.channel("colour"):
        out["pbrMetallicRoughness"]["baseColorFactor"] = [float(c) for c in colour]
    for sampler in material.samplers:
        if sampler.dynamic or sampler.lightmap or sampler.palette_index >= len(palette) or palette[sampler.palette_index] is None:
            continue
        try:
            png, normal = textures.write(palette[sampler.palette_index], mask=sampler.channel == "transparency")
        except ValueError:
            continue
        if normal:
            out["normalTexture"] = {"index": texture_of(png)}
        elif sampler.channel == "colour" and "baseColorTexture" not in out["pbrMetallicRoughness"]:
            out["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture_of(png)}
        elif sampler.channel == "transparency":
            out["alphaMode"] = "MASK"
    return out
