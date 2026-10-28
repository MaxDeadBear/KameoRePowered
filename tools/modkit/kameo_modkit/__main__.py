"""Command line: python -m kameo_modkit info <file> | obj <file> <out.obj> | gltf <file> <out.gltf>
| import <original.mdl> <edited.gltf|glb> <out.mdl> | kmod <out.kmod> <original>=<edited>... --name <name>"""

from __future__ import annotations

import argparse
import os
from collections import Counter

import numpy as np

from .caff import Caff
from .graph import Ref, Resolver, rendergraph_roots
from .material import read_material, texture_palette
from .mesh import NORMAL, TEXCOORD, collect_meshes
from .texture import read_texture, to_pixels, write_png


def info(path: str) -> None:
    archive = Caff.load(path)
    resolver = Resolver(archive)
    print(f"{path}: {len(archive.entries)} entries, {sum(len(r.offsets) for r in archive.relocations)} pointers")
    for section in archive.sections:
        print(f"  section {section.name}: {section.size} bytes")
    types = Counter(resolver.type_name(entry) or f"<{archive.sections[entry.section - 1].name} data>" for entry in archive.entries)
    for name, count in types.most_common():
        print(f"  {count:5d} {name}")


def _normal_map(pixels: np.ndarray) -> np.ndarray:
    """A two-channel DXN normal map with its Z rebuilt, as Blender expects a tangent-space normal map."""
    nx = pixels[:, :, 0] / 127.5 - 1.0
    ny = pixels[:, :, 1] / 127.5 - 1.0
    nz = ((np.sqrt(np.maximum(0.0, 1.0 - nx * nx - ny * ny)) * 0.5 + 0.5) * 255).astype(np.uint8)
    return np.stack([pixels[:, :, 0], pixels[:, :, 1], nz], axis=2)


class TextureWriter:
    def __init__(self, resolver: Resolver, folder: str, relative: str):
        self.resolver = resolver
        self.folder = folder
        self.relative = relative
        self.written: dict[tuple[int, int], tuple[str, bool]] = {}
        os.makedirs(folder, exist_ok=True)

    def write(self, ref: Ref, mask: bool = False) -> tuple[str, bool]:
        """The texture's PNG path relative to the OBJ, and whether it is a normal map; a mask is written as white with its brightness as alpha."""
        key = (ref.entry.index, ref.offset, mask)
        if key not in self.written:
            texture = read_texture(self.resolver, ref)
            suffix = "_mask" if mask else ""
            name = f"tex{len(self.written):03d}_{texture.width}x{texture.height}_{texture.format_name}{suffix}.png"
            pixels = to_pixels(self.resolver, texture)
            normal = texture.format_name == "DXN"
            if normal:
                pixels = _normal_map(pixels)
            elif mask:
                # White, its brightness as alpha (Pillow's L: 299, 587 and 114 thousandths).
                luma = (pixels[:, :, 0].astype(np.uint32) * 299 + pixels[:, :, 1].astype(np.uint32) * 587 + pixels[:, :, 2].astype(np.uint32) * 114) // 1000
                pixels = np.stack([np.full_like(pixels[:, :, 0], 255)] * 3 + [luma.astype(np.uint8)], axis=2)
            write_png(os.path.join(self.folder, name), pixels)
            self.written[key] = (f"{self.relative}/{name}", normal)
        return self.written[key]


def export_obj(path: str, out: str) -> None:
    archive = Caff.load(path)
    resolver = Resolver(archive)
    stem = os.path.splitext(os.path.basename(out))[0]
    folder = os.path.dirname(os.path.abspath(out))
    textures = TextureWriter(resolver, os.path.join(folder, f"{stem}_textures"), f"{stem}_textures")
    materials: dict[tuple[int, int], str] = {}
    material_lines: list[str] = []
    base = 1
    with open(out, "w", encoding="ascii") as handle:
        handle.write(f"mtllib {stem}.mtl\n")
        for graph_index, entry in enumerate(e for e in archive.entries if resolver.type_name(e) == "rendergraph"):
            palette = texture_palette(resolver, Ref(entry, 0))
            for mesh_index, mesh in enumerate(collect_meshes(resolver, rendergraph_roots(resolver, entry))):
                positions = mesh.positions
                uvs = mesh.uvs() if (TEXCOORD, 0) in mesh.attributes else []
                normals = mesh.attributes.get((NORMAL, 0), [])
                handle.write(f"o graph{graph_index}_mesh{mesh_index}\n")
                for x, y, z in positions:
                    handle.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
                # One texture coordinate and normal per vertex, even for a mesh without them, so indices stay in step.
                for i in range(len(positions)):
                    u, v = uvs[i] if i < len(uvs) else (0.0, 0.0)
                    handle.write(f"vt {u:.6f} {1.0 - v:.6f}\n")
                for i in range(len(positions)):
                    nx, ny, nz = normals[i][:3] if i < len(normals) else (0.0, 1.0, 0.0)
                    handle.write(f"vn {nx:.5f} {ny:.5f} {nz:.5f}\n")
                for draw in mesh.draws:
                    if draw.material is not None:
                        key = (draw.material.ref.entry.index, draw.material.ref.offset)
                        if key not in materials:
                            materials[key] = f"graph{graph_index}_material{len(materials)}"
                            material_lines.extend(_material_block(resolver, draw.material, materials[key], palette, textures))
                        handle.write(f"usemtl {materials[key]}\n")
                    for a, b, c in draw.triangles():
                        if max(a, b, c) >= len(positions):
                            continue
                        handle.write(f"f {a + base}/{a + base}/{a + base} {b + base}/{b + base}/{b + base} {c + base}/{c + base}/{c + base}\n")
                base += len(positions)
    with open(os.path.join(folder, f"{stem}.mtl"), "w", encoding="ascii") as handle:
        handle.write("\n".join(material_lines) + "\n")
    print(f"wrote {out}, {len(materials)} materials, {len(textures.written)} textures")


def _material_block(resolver: Resolver, node, name: str, palette: list, textures: TextureWriter) -> list[str]:
    material = read_material(resolver, node)
    red, green, blue, _ = (1.0, 1.0, 1.0, 1.0) if material.channel("colour") else material.base_colour() or (1.0, 1.0, 1.0, 1.0)
    lines = [f"newmtl {name}", f"Kd {red:.4f} {green:.4f} {blue:.4f}", "Ks 0.0 0.0 0.0", "d 1.0", "illum 2"]
    for sampler in material.samplers:
        if sampler.dynamic or sampler.lightmap or sampler.palette_index >= len(palette) or palette[sampler.palette_index] is None:
            continue
        try:
            png, normal = textures.write(palette[sampler.palette_index], mask=sampler.channel == "transparency")
        except ValueError:
            continue
        if normal:
            lines.append(f"map_Bump {png}")
        elif sampler.channel == "colour":
            lines.append(f"map_Kd {png}")
        elif sampler.channel == "transparency":
            lines.append(f"map_d {png}")
        elif sampler.channel == "specular":
            lines.append(f"map_Ks {png}")
    lines.append("")
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(prog="kameo_modkit")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("info").add_argument("file")
    obj = commands.add_parser("obj")
    obj.add_argument("file")
    obj.add_argument("out")
    gltf = commands.add_parser("gltf")
    gltf.add_argument("file")
    gltf.add_argument("out")
    imp = commands.add_parser("import")
    imp.add_argument("original")
    imp.add_argument("gltf")
    imp.add_argument("out")
    imp.add_argument("--keep-missing", action="store_true", help="keep the old geometry of meshes the glTF has no triangles for")
    pack = commands.add_parser("kmod", help="package edited archives as a mod for the launcher's Mods page")
    pack.add_argument("out")
    pack.add_argument("pairs", nargs="+", metavar="original=edited", help="a shipped archive and its edited copy")
    pack.add_argument("--name", required=True)
    pack.add_argument("--author", default="")
    pack.add_argument("--version", default="")
    pack.add_argument("--description", default="")
    args = parser.parse_args()
    if args.command == "kmod":
        from . import kmod

        files = []
        for pair in args.pairs:
            original, _, edited = pair.partition("=")
            with open(original, "rb") as a, open(edited, "rb") as b:
                files.append(kmod.diff(os.path.basename(original), a.read(), b.read()))
        with open(args.out, "wb") as handle:
            handle.write(kmod.write(files, args.name, args.author, args.version, args.description))
        print(f"wrote {args.out}: " + ", ".join(f"{f.name} ({sum(len(r) for _, runs in f.sections for _, r in runs)} bytes changed)" for f in files))
        return
    if args.command == "import":
        from .importer import ImportError_, import_gltf

        try:
            notes = import_gltf(args.original, args.gltf, args.out, args.keep_missing)
        except ImportError_ as error:
            raise SystemExit(f"import failed: {error}")
        for note in notes:
            print(f"note: {note}")
        print(f"wrote {args.out}")
    elif args.command == "info":
        info(args.file)
    elif args.command == "gltf":
        from .gltf import export_gltf

        export_gltf(args.file, args.out)
    else:
        export_obj(args.file, args.out)


if __name__ == "__main__":
    main()
