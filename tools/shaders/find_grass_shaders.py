#!/usr/bin/env python3
"""Lists the shaders the levels' grass draws with, for the shader mods' "Grass and plants" target: background materials
(and the plant props: grass, ferns, reeds, flowers, weeds) whose colour texture is green, by the pixel shaders of the
ndShader2 above them (a table at +216 to +276, one per lighting variant). The same shaders also draw rock and earth,
so a grass mod tells grass from them by colour.

    python tools/shaders/find_grass_shaders.py <game assets folder> [tools/shaders/grass_shaders.json]

build_shader_mod_source.py reads the list. Shaders that draw the grass from game code rather than a rendergraph
(the Enchanted Kingdom's tall grass, PLiteGrass) are found in the running game instead, with
kameo_dev_find_texture_shaders, and listed in EXTRA.
"""
import json
import os
import struct
import sys
import zipfile
from collections import defaultdict

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "shaders"))
sys.path.insert(0, os.path.join(ROOT, "tools", "modkit"))
sys.path.insert(0, os.path.join(ROOT, "tools", "textures"))
from build_shader_mod_source import containers_of  # noqa: E402
from kameo_modkit.caff import Caff  # noqa: E402
from kameo_modkit.graph import Ref, Resolver, rendergraph_roots  # noqa: E402
from kameo_modkit.material import read_material, texture_palette  # noqa: E402
from kameo_modkit.texture import read_texture, to_pixels  # noqa: E402
from kameo_modkit import xmlblock as X  # noqa: E402

assets = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "grass_shaders.json")
# Found in the game: vtxShaderDryGrass (PLiteGrass), the tall grass.
EXTRA: list[str] = ["0C55435391DE66E3"]
SHARED = "011C2CC3.lvl"
PLANTS = ("grass", "fern", "reed", "flower", "weed")


def is_green(rgb) -> bool:
    r, g, b = rgb
    return g > 35 and g > r * 1.08 and g > b * 1.2


def has_background(nodes) -> bool:
    return any(n.type == "ndBGPushBuffer" or has_background(n.children) for n in nodes)


grass, other = set(), set()
for name in sorted(os.listdir(assets)):
    if not name.endswith(".lvl") or name == SHARED:
        continue
    archive = Caff.load(os.path.join(assets, name))
    resolver = Resolver(archive)
    found = containers_of(archive)
    at = {offset: shader for shader, (_, offset) in found.items()}
    graphs = [e for e in archive.entries if resolver.type_name(e) == "rendergraph"]
    wanted = {e.index for e in graphs if has_background(rendergraph_roots(resolver, e))}
    links = defaultdict(set)
    for r in archive.relocations:
        links[r.source].add(r.target)
    try:
        types = X.database_types(archive)
    except Exception:
        types = []
    for t in types:
        if not any(p in t.name.lower() for p in PLANTS):
            continue
        todo, seen = [e.index for (_, _, e) in t.assets if e is not None], set()
        while todo:
            i = todo.pop()
            if i in seen:
                continue
            seen.add(i)
            kind = resolver.type_name(archive.entries[i - 1])
            if kind in ("kameoDatabase", "kameolevel"):
                continue
            if kind == "rendergraph":
                wanted.add(i)
                continue
            todo += list(links[i])
    # Per graph: the relocated fields, to find the shader an ndShader2 points at.
    fields = defaultdict(list)
    for r in archive.relocations:
        for offset in r.offsets:
            fields[r.source].append((offset, r.target))
    colour_of = {}
    for g in wanted:
        entry = archive.entries[g - 1]
        data = resolver.data(entry)
        palette = texture_palette(resolver, Ref(entry, 0))

        # An ndShader2 holds a table of pixel shaders, one per lighting variant, at +216 to +276.
        def shader_at(node):
            shaders = set()
            for offset, target in fields[g]:
                if node.ref.offset + 200 <= offset < node.ref.offset + 288:
                    t = archive.entries[target - 1]
                    shader = at.get(t.offset + struct.unpack_from(">I", data, offset)[0])
                    if shader and archive.sections[t.section - 1].name == ".data":
                        shaders.add(shader)
            return shaders

        def visit(nodes, shader):
            for n in nodes:
                s = shader
                if n.type == "ndShader2":
                    s = shader_at(n) or shader
                elif n.type.startswith("ndShaderParam2") and s:
                    m = read_material(resolver, n)
                    sampler = m.channel("colour")
                    texture = palette[sampler.palette_index] if sampler and sampler.palette_index < len(palette) else None
                    if texture is not None:
                        key = (texture.entry.index, texture.offset)
                        if key not in colour_of:
                            try:
                                px = to_pixels(resolver, read_texture(resolver, texture)).astype(np.float32)
                                a = px[..., 3:4] / 255.0
                                colour_of[key] = (tuple((px[..., :3] * a).sum((0, 1)) / max(a.sum(), 1.0)), px)
                            except Exception:
                                colour_of[key] = (None, None)
                        rgb, px = colour_of[key]
                        if rgb is not None and is_green(rgb):
                            grass.update(s)
                        else:
                            other.update(s)
                visit(n.children, s)

        visit(rendergraph_roots(resolver, entry), None)
    print(name, "grass shaders so far", len(grass), flush=True)


with zipfile.ZipFile(os.path.join(ROOT, "res", "shader_mods", "source.zip")) as pack:
    targets = json.loads(pack.read("targets.json"))
    available = set(pack.namelist())
characters = set()
for key, shaders in targets.items():
    if key in ("kameo", "kameo_everyday") or key.endswith("_standard") or key in ("flowerboxer", "spitter", "rubble", "armadillo", "watercreature_small", "watercannon", "snow_man", "ice_b_yeti", "fire_ant", "firedragon"):
        characters |= set(shaders)
result = sorted(s for s in (grass | set(EXTRA)) - characters if f"{s}.hlsl" in available)
print("grass shaders:", len(grass), "also used for other surfaces:", len(grass & other), "less characters:", len(result))
json.dump(result, open(out, "w"), indent=0)

