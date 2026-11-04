#!/usr/bin/env python3
"""Build res/shader_mods/source.zip: every pixel shader in the game, ready to take a shader mod.

Run after the game's shaders change. It needs a built XenosRecomp, the shader_dump library and the game's archives.

    python tools/shaders/build_shader_mod_source.py --assets out/build/release/assets \\
        --dlc "%USERPROFILE%/Documents/kameorepowered"

Each shader is translated to HLSL, then given calls into src/gpu/shaders/hlsl/shader_mod.hlsli at the places a mod's
Shade function reads from. Those places come from what the code does, since a shader's samplers change role between
the game's permutations (a normal map can sit in s_sampler0 and the colour in s_sampler1):

    the fog amount  what the fog colour is mixed in by; Shade runs at the very end, as shaders add to their colour after
                    fog, and would paint over what it returned earlier, so fog goes back on over its result
    the normal      what is dotted with c_light0dir or c_invview1
    the view        the position fog is worked out from
    the UV          what the first plain 2D texture read is given
    albedo          the shader itself, run a second time in a white, unshadowed, fogless world, which leaves its textures
                    with every layer and tint a material has; a mod that doesn't read albedo compiles this copy away

The zip also holds targets.json: which shaders each level and character uses, by the names in textures/sort_textures.py. A
character's are those of its model files, its DLC skins and its standard look, a rendergraph of the shared archive.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "textures"))
sys.path.insert(0, os.path.join(ROOT, "tools", "modkit"))

from kameo_modkit.caff import Caff  # noqa: E402
from kameo_modkit.graph import Ref, Resolver, rendergraph_roots  # noqa: E402
from sort_textures import area_of  # noqa: E402

import xxhash  # noqa: E402

RECOMP = os.path.join(ROOT, "thirdparty", "XenosRecomp", "build", "XenosRecomp", "Release", "XenosRecomp.exe")
COMMON = os.path.join(ROOT, "thirdparty", "XenosRecomp", "XenosRecomp", "shader_common.h")
TEMPLATE = os.path.join(ROOT, "src", "gpu", "shaders", "hlsl", "shader_mod.hlsli")
DUMPS = os.path.join(ROOT, "shader_dump")
OUTPUT = os.path.join(ROOT, "res", "shader_mods", "source.zip")

CONTAINER = re.compile(rb"\x10\x2A\x0E\x00")
SHARED_ARCHIVE = "011C2CC3.lvl"
EVERYDAY_TARGET = "kameo_everyday"
# Kinds of surface, by the shaders that draw them, found with the game's F7 shader list and the
# kameo_dev_find_texture_shaders log. Some are unlit, so a mod for them is built with MOD_INCLUDE_UNLIT.
SURFACES = {"lava": [
    "019C91C2AC8436A7",
    "049D8B13CFC489E5",
    "31F5F851F6841602",
    "3A04EBDFDE2633A9",
    "42477341E4D3F511",
    "55C19895B82426FC",
    "5C5057C59A9B581E",
    "70189F822F1AA455",
    "74980142E87993E6",
    "86634CC00F646AAE",
    "B80FD95F69DFFC50",
    "D8D32630B274DFF1",
    "F1723B429B3EBFF1",
    "F709021AFC457795",
], "fire": [
    "13FFE0BD01E855A3",
]}
# The levels' grass, ferns and leaves, lit: listed by find_grass_shaders.py from the green background materials.
_GRASS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grass_shaders.json")
if os.path.exists(_GRASS):
    with open(_GRASS) as _grass:
        SURFACES["grass"] = json.load(_grass)
# Each character a shader mod can make a costume for, by the model type its skins are listed under in the game: a
# costume target is that character's standard look alone, Kameo's everyday outfit for her.
COSTUME_MODELS = {"kameo": "007183BF", "flowerboxer": "0CE65812", "spitter": "0A70BAB2", "rubble": "079B8925", "armadillo": "037AABBF", "watercreature_small": "005C444C", "watercannon": "08EBDF9E", "snow_man": "056D64DE", "ice_b_yeti": "05800519", "fire_ant": "008B5284", "firedragon": "0BBEAEDE"}
MESH_KEY_BYTES = 256  # kShaderModMeshKeyBytes
# Each character's standard look: its rendergraph's position in the shared archive, matched by skeleton to its skins.
STANDARD_GRAPHS = {"kameo": 1, "flowerboxer": 8, "spitter": 36, "rubble": 27, "armadillo": 10, "watercreature_small": 34, "watercannon": 43, "snow_man": 48, "ice_b_yeti": 12, "fire_ant": 39, "firedragon": 29}
LIGHTS = re.compile(r"c_ambient|c_ambientHigh|c_fillColour|c_light\dcolour|c_shadowCol|c_shadLightDiffColour")
# The step that mixes fog in: a colour times a single channel, the share of it fog leaves, plus the fog's colour.
FOG = re.compile(r"= (?:r\d+\.[xyzw]{3} \* (r\d+)\.([xyzw])\2\2|(r\d+)\.([xyzw])\4\4 \* r\d+\.[xyzw]{3}) \+ c_fogcolour\.[xyz]{3};")
# What the second run of a shader sees: all ambient and no other light, no shadow and no fog (build_neutral_copy).
NEUTRAL = {
    "c_ambient": "MOD_WHITE", "c_ambientHigh": "MOD_WHITE", "c_fillColour": "MOD_BLACK", "c_light0colour": "MOD_BLACK",
    "c_light1colour": "MOD_BLACK", "c_light2colour": "MOD_BLACK", "c_shadowCol": "MOD_BLACK", "c_shadLightDiffColour": "MOD_BLACK",
    "c_shadowFade": "MOD_NO_SHADE", "c_fog": "MOD_NO_FOG",
}
NEUTRAL_NAMES = re.compile(r"\b(" + "|".join(NEUTRAL) + r")\b")
NORMAL = re.compile(r"dot\(c_(?:light0dir|invview1)\.([xyz]{3}), (r\d+)\.([xyzw]{3})\)|dot\((r\d+)\.([xyzw]{3}), c_(?:light0dir|invview1)\.([xyz]{3})\)")
POSITION = re.compile(r"(r\d+)\.z \* c_fog\.x")
UV = re.compile(r"tfetch2D\(\w+, \w+, (r\d+)\.xy, float2\(0, 0\)\)")
ALPHA_TEST = "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
START_MARK = "CubeMapData cubeMapData = (CubeMapData)0;"
# After the mod's shade.hlsl, so a mod can define MOD_FETCH2D first.
FETCH_DEFAULT = "// A mod may read textures its own way by defining MOD_FETCH2D in shade.hlsl (shader_mod.hlsli).\n#ifndef MOD_FETCH2D\n#define MOD_FETCH2D tfetch2D\n#endif\n"


def containers_of(archive: Caff) -> dict[str, tuple[bytes, int]]:
    """Every pixel shader container in an archive, by the hash the game looks it up with, with where it sits in .data."""
    data = b"".join(section.data for section in archive.sections if section.name == ".data")
    found = {}
    for match in CONTAINER.finditer(data):
        offset = match.start()
        if offset + 12 > len(data):
            continue
        virtual_size, physical_size = struct.unpack(">II", data[offset + 4:offset + 12])
        total = virtual_size + physical_size
        if 0 < total <= 0x100000 and offset + total <= len(data):
            blob = data[offset:offset + total]
            found[f"{xxhash.xxh3_64_intdigest(blob):016X}"] = (blob, offset)
    return found


def graph_shaders(archive: Caff, found: dict[str, tuple[bytes, int]], position: int) -> set[str]:
    """The shaders a rendergraph draws with, which it points at directly; an archive keeps all its shaders in one entry."""
    resolver = Resolver(archive)
    graph = [entry for entry in archive.entries if resolver.type_name(entry) == "rendergraph"][position]
    at = {offset: shader for shader, (_, offset) in found.items()}
    data = resolver.data(graph)
    shaders = set()
    for relocation in archive.relocations:
        if relocation.source != graph.index:
            continue
        target = archive.entries[relocation.target - 1]
        for field in relocation.offsets:
            shader = at.get(target.offset + struct.unpack_from(">I", data, field)[0])
            if shader and archive.sections[target.section - 1].name == ".data":
                shaders.add(shader)
    return shaders


def build_neutral_copy(lines: list[str], start: int, finish: int) -> list[str]:
    """The shader over again in a block of its own, whose output is the surface's colour with no light on it."""
    open_body = next(i for i in range(start, -1, -1) if lines[i].strip() == "{")
    copy = []
    for line in lines[open_body + 1:start + 1] + lines[start + 1:finish]:
        line = NEUTRAL_NAMES.sub(lambda match: NEUTRAL[match.group(1)], line)
        # Only the real run may throw a pixel away.
        line = re.sub(r"\bclip\([^;]*\);|\bdiscard;", ";", line)
        copy.append(re.sub(r"\boC0\b", "mod_base", line))
    return ["float4 mod_base = 0.0;", "{", *copy, "}", "mod.s.albedo = mod_base.xyz; mod.s.has_albedo = true;"]


def graph_meshes(archive: Caff, position: int) -> list[str]:
    """Keys for a rendergraph's vertex buffers, which tell its draws from any other model's using the same shaders."""
    resolver = Resolver(archive)
    graph = [entry for entry in archive.entries if resolver.type_name(entry) == "rendergraph"][position]
    keys = set()

    def visit(nodes) -> None:
        for node in nodes:
            if node.type in ("ndVertexBuffer2", "ndBlendShape2"):
                table = resolver.pointer(node.ref, 48)
                for i in range(resolver.u32(node.ref, 56) if table else 0):
                    record = Ref(table.entry, table.offset + 20 * i)
                    data = resolver.pointer(record, 12)
                    if data and resolver.u32(record, 16) >= MESH_KEY_BYTES:
                        keys.add(f"{xxhash.xxh3_64_intdigest(resolver.bytes(data, MESH_KEY_BYTES)):016X}")
            visit(node.children)

    visit(rendergraph_roots(resolver, graph))
    return sorted(keys)


def inject(text: str, common: str) -> tuple[str, Counter]:
    """Adds a mod's calls to a translated shader; the counter says which inputs it could find."""
    if not text.startswith(common):
        raise ValueError("the translation doesn't start with shader_common.h")
    lines = text[len(common):].split("\n")
    start = next(i for i, line in enumerate(lines) if START_MARK in line)
    finish = next(i for i, line in reversed(list(enumerate(lines))) if ALPHA_TEST in line)
    if sum("return;" in line for line in lines[start:]) != 1:
        raise ValueError("more than one way out of main")

    normal = position = fog = uv = None
    for i in range(start + 1, finish):
        line = lines[i]
        if normal is None:
            match = NORMAL.search(line)
            if match:
                order, register, swizzle = match.groups()[:3] if match.group(1) else (match.group(6), match.group(4), match.group(5))
                normal = (i, register, swizzle, order)
        if position is None:
            match = POSITION.search(line)
            if match:
                position = (i, match.group(1))
        if uv is None:
            match = UV.search(line)
            if match:
                uv = (i, match.group(1))
        match = FOG.search(line)
        if match:
            register, channel = [group for group in match.groups() if group]
            fog = (i, f"{register}.{channel}")

    body = "\n".join(lines[start + 1:])
    is_lit = bool(LIGHTS.search(body))
    setup = [f"ModState mod = ModStart(iPos.xy, g_ModTime);", f"mod.lit = {'true' if is_lit else 'false'};"]
    if "c_invview1" in body:
        setup.append("mod.s.up = c_invview1.xyz;")
    if "c_light0dir" in body:
        setup.append("mod.s.light_dir = c_light0dir.xyz;")
    if "c_light0colour" in body:
        setup.append("mod.s.light_colour = c_light0colour.xyz;")

    if is_lit:
        setup.append("mod.derive_light = true;")
        # Before the edits below: the copy keeps the shader's own fog step.
        setup += build_neutral_copy(lines, start, finish)
    setup.append("float3 mod_rgb;")
    # Latest place first, so the earlier indices stay right.
    edits = [(finish, "before", "oC0.xyz = ModFinish(mod, oC0.xyz, oC0.w);")]
    if fog:
        edits.append((fog[0], "before", f"mod.fog_colour = c_fogcolour.xyz; mod.fog_amount = {fog[1]}; mod.has_fog = true;"))
    if normal:
        index, register, swizzle, order = normal
        edits.append((index, "before", f"mod_rgb.{order} = {register}.{swizzle}; mod.s.normal = ModNormalize(mod_rgb, mod.s.normal); mod.s.has_normal = true;"))
    if position:
        edits.append((position[0], "before", f"mod.s.view = ModNormalize(-{position[1]}.xyz, mod.s.view);"))
    if uv:
        edits.append((uv[0], "before", f"mod.s.uv = {uv[1]}.xy;"))
    edits.append((start, "after", "\n\t".join(setup)))
    for index, side, code in sorted(edits, key=lambda edit: (edit[0], edit[1] == "before"), reverse=True):
        lines.insert(index + (side == "after"), "\t" + code)

    found = Counter(lit=is_lit, fog=fog is not None, normal=normal is not None, view=position is not None)
    # Texture reads go through MOD_FETCH2D, which a mod's shade.hlsl may define, as tfetch2D otherwise.
    body = "\n".join(lines).replace("tfetch2D(", "MOD_FETCH2D(")
    return '#include "shader_common.h"\n#include "shader_mod.hlsli"\n#include "shade.hlsl"\n' + FETCH_DEFAULT + body, found


def patched_common(common: str) -> str:
    """shader_common.h with c17.w, c19 and c20.x, which the game fills for shader mods alone."""
    vulkan = "#define g_StereoShift              vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 276)\n"
    direct = "    float2 g_StereoShift : packoffset(c17.y);\n"
    if vulkan not in common or direct not in common:
        raise ValueError("shader_common.h has changed: update patched_common")
    # c17.w the time; c19 the bindless indices of the game's textures a mod draws with, c20.x their sampler, c20.y how
    # a see-through mod shows (MOD_SHOW_* in shader_mod.hlsli).
    common = common.replace(vulkan, vulkan + "#define g_ModTime                  vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 284)\n"
                            "#define g_ModTextures              vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 304)\n"
                            "#define g_ModSampler               vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320)\n"
                            "#define g_ModShow                  vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 324)\n")
    return common.replace(direct, "    float2 g_StereoShift : packoffset(c17.y); \\\n    float g_ModTime : packoffset(c17.w); \\\n"
                          "    uint4 g_ModTextures : packoffset(c19); \\\n    uint g_ModSampler : packoffset(c20.x); \\\n"
                          "    uint g_ModShow : packoffset(c20.y);\n")


# Shaders whose constants have names of their own, which inject() can't find its places by, hooked by hand after it.
# The tall grass (PLiteGrass, found with kameo_dev_find_texture_shaders): lit by SunLightColour and AmbientLightColour,
# fogged by FogColour, its colour the diffuse texture times the vertex colour, read at r0 (r1 is the shadow map's).
HAND_HOOKS = {
    "0C55435391DE66E3": [
        ("\tmod.lit = false;", "\tmod.lit = true;\n\tmod.derive_light = true;"),
        ("\tmod.s.uv = r1.xy;", "\tmod.s.uv = r0.xy;"),
        ("\tr5.xyzw = MOD_FETCH2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyzw;",
         "\tr5.xyzw = MOD_FETCH2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyzw;\n"
         "\tmod.s.albedo = r5.xyz * r3.xyz; mod.s.has_albedo = true;"),
        ("\toC0.xyz = r1.xyz * r0.yyy + FogColour.xyz;",
         "\tmod.fog_colour = FogColour.xyz; mod.fog_amount = r0.y; mod.has_fog = true;\n\toC0.xyz = r1.xyz * r0.yyy + FogColour.xyz;"),
    ],
}


def hand_hooks(shader: str, source: str) -> str:
    for old, new in HAND_HOOKS.get(shader, []):
        if source.count(old) != 1:
            raise ValueError(f"{shader}: its hand hooks no longer match the translation")
        source = source.replace(old, new)
    return source


def costume_target(character: str) -> str:
    """The target a costume for one character is built from."""
    return EVERYDAY_TARGET if character == "kameo" else f"{character}_standard"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--assets", required=True, help="the game's assets folder")
    parser.add_argument("--dlc", help="the folder DLC is installed under, usually Documents/kameorepowered")
    parser.add_argument("--out", default=OUTPUT)
    parser.add_argument("--threads", type=int, default=max(2, (os.cpu_count() or 4) - 2))
    arguments = parser.parse_args()

    targets: dict[str, set[str]] = {}
    meshes: dict[str, list[str]] = {}
    costumes: dict[str, str] = {}
    containers: dict[str, bytes] = {}
    for name in sorted(os.listdir(arguments.assets)):
        if not name.lower().endswith((".lvl", ".mdl")):
            continue
        archive = Caff.load(os.path.join(arguments.assets, name))
        found = containers_of(archive)
        containers.update({shader: blob for shader, (blob, _) in found.items()})
        targets.setdefault(area_of(f"game:{name}"), set()).update(found)
        if name == SHARED_ARCHIVE:
            for character, position in STANDARD_GRAPHS.items():
                targets.setdefault(character, set()).update(graph_shaders(archive, found, position))
            for character, model in COSTUME_MODELS.items():
                target = costume_target(character)
                targets[target] = graph_shaders(archive, found, STANDARD_GRAPHS[character])
                meshes[target] = graph_meshes(archive, STANDARD_GRAPHS[character])
                costumes[target] = model
    if arguments.dlc:
        root = os.path.expandvars(os.path.join(arguments.dlc, "0000000000000000", "4D5307D2", "00000002"))
        for folder, _, files in os.walk(root):
            for name in files:
                if not name.lower().endswith((".lvl", ".mdl")):
                    continue
                found = containers_of(Caff.load(os.path.join(folder, name)))
                containers.update({shader: blob for shader, (blob, _) in found.items()})
                # A DLC skin is named like the model it dresses; its levels have names no game file does.
                owner = area_of(f"game:{name}")
                targets.setdefault("DLC levels" if owner == os.path.splitext(name)[0] else owner, set()).update(found)
    for surface, shaders in SURFACES.items():
        targets[surface] = set(shaders)
        containers.update({shader: b"" for shader in shaders if shader not in containers})
    # A shader the game's cache lacks can't be drawn, modded or not.
    known = {name[:16] for name in os.listdir(DUMPS)}
    missing = set(containers) - known
    containers = {shader: blob for shader, blob in containers.items() if shader in known}
    targets = {name: shaders & known for name, shaders in targets.items()}
    print(f"{len(containers)} pixel shaders in {len(targets)} targets; {len(missing)} left out, not in the game's cache", flush=True)

    with open(COMMON, encoding="utf-8", newline="") as handle:
        common = handle.read()

    def translate(item: tuple[str, str]) -> tuple[str, str]:
        shader, folder = item
        output = os.path.join(folder, f"{shader}.hlsl")
        subprocess.run([RECOMP, os.path.join(DUMPS, f"{shader}.bin"), output, COMMON], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        with open(output, encoding="utf-8", newline="") as handle:
            return shader, handle.read()

    tally: Counter = Counter()
    os.makedirs(os.path.dirname(arguments.out), exist_ok=True)
    with tempfile.TemporaryDirectory() as folder, ThreadPoolExecutor(arguments.threads) as pool, zipfile.ZipFile(arguments.out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as pack:
        pack.writestr("shader_common.h", patched_common(common))
        with open(TEMPLATE, encoding="utf-8") as handle:
            pack.writestr("shader_mod.hlsli", handle.read())
        pack.writestr("targets.json", json.dumps({name: sorted(shaders) for name, shaders in sorted(targets.items())}))
        pack.writestr("meshes.json", json.dumps(meshes))
        pack.writestr("costumes.json", json.dumps(costumes))
        for shader, text in pool.map(translate, [(shader, folder) for shader in sorted(containers)]):
            source, found = inject(text, common)
            source = hand_hooks(shader, source)
            tally.update(found)
            pack.writestr(f"{shader}.hlsl", source)
    print(f"{dict(tally)} in {os.path.getsize(arguments.out) / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
