#!/usr/bin/env python3
"""Compile a shader mod from the command line, without the editor.

    python tools/shaders/build_shader_mod.py shade.hlsl out/build/release/shader_mods/my_mod.kshader --targets kameo,rubble

shade.hlsl defines `float3 Shade(ShadeInputs s)` (src/gpu/shaders/hlsl/shader_mod.hlsli). A target is a name from
res/shader_mods/source.zip's targets.json, a level or a monster; `characters` is every model file and `everything`
every shader in the game. A target covers shaders, so whatever else draws with those shaders changes as well.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DXC = os.path.join(ROOT, "thirdparty", "XenosRecomp", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
SOURCE = os.path.join(ROOT, "res", "shader_mods", "source.zip")
MODELS = ("armadillo", "fire_ant", "firedragon", "flowerboxer", "ice_b_yeti", "kameo", "rubble", "snow_man", "spitter", "watercannon", "watercreature_small")


def wanted(table: dict[str, list[str]], names: list[str]) -> list[str]:
    shaders: set[str] = set()
    for name in names:
        if name == "everything":
            shaders.update(shader for group in table.values() for shader in group)
        elif name == "characters":
            shaders.update(shader for model in MODELS for shader in table[model])
        elif name in table:
            shaders.update(table[name])
        else:
            raise SystemExit(f"no target called {name}; there are {', '.join(sorted(table))}, characters and everything")
    return sorted(shaders)


def compile_one(folder: str, shader: str, spirv: bool) -> tuple[bytes, str]:
    output = os.path.join(folder, f"{shader}.{'spv' if spirv else 'dxil'}")
    flags = ["-spirv", "-fvk-use-dx-layout"] if spirv else ["-Wno-ignored-attributes", "-Qstrip_reflect"]
    result = subprocess.run([DXC, "-T", "ps_6_0", "-HV", "2021", "-all-resources-bound", *flags, "-Qstrip_debug", "-Fo", output, os.path.join(folder, f"{shader}.hlsl")], capture_output=True, text=True)
    if result.returncode != 0:
        return b"", result.stderr
    with open(output, "rb") as handle:
        return handle.read(), ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("shade", help="the file defining Shade")
    parser.add_argument("output", help="the .kshader to write")
    parser.add_argument("--targets", default="characters", help="comma-separated targets")
    parser.add_argument("--dxil-only", action="store_true", help="leave out Vulkan, for quicker test builds")
    parser.add_argument("--meshes", help="limit the mod to a meshes.json entry; by default a lone target that has one uses it")
    parser.add_argument("--threads", type=int, default=max(2, (os.cpu_count() or 4) - 2))
    arguments = parser.parse_args()

    with zipfile.ZipFile(SOURCE) as source, tempfile.TemporaryDirectory() as folder:
        shaders = wanted(json.loads(source.read("targets.json")), arguments.targets.split(","))
        mesh_table = json.loads(source.read("meshes.json"))
        # A character's standard look alone makes a costume for that character; older packs only had Kameo's.
        costume_table = json.loads(source.read("costumes.json")) if "costumes.json" in source.namelist() else {"kameo_everyday": "007183BF"}
        costume = arguments.meshes or arguments.targets
        meshes = [int(mesh, 16) for mesh in mesh_table.get(costume, [])] if costume in costume_table else []
        model_type = int(costume_table[costume], 16) if meshes else 0
        for name in ["shader_common.h", "shader_mod.hlsli"] + [f"{shader}.hlsl" for shader in shaders]:
            source.extract(name, folder)
        with open(arguments.shade, encoding="utf-8") as handle, open(os.path.join(folder, "shade.hlsl"), "w", encoding="utf-8") as shade:
            shade.write(handle.read())

        with ThreadPoolExecutor(arguments.threads) as pool:
            dxil = list(pool.map(lambda shader: compile_one(folder, shader, False), shaders))
            spirv = [(b"", "")] * len(shaders) if arguments.dxil_only else list(pool.map(lambda shader: compile_one(folder, shader, True), shaders))
        failed = [(shader, error) for shader, (blob, error) in zip(shaders, dxil + spirv) if error]
        for shader, error in failed[:3]:
            print(f"{shader}:\n{error}", file=sys.stderr)
        if failed:
            print(f"{len(failed)} compiles failed", file=sys.stderr)
            return 1

    blobs = bytearray()
    entries = []
    base = 20 + 24 * len(shaders) + 8 * len(meshes)
    for shader, (dxil_blob, _), (spirv_blob, _) in zip(shaders, dxil, spirv):
        dxil_offset = base + len(blobs)
        blobs += dxil_blob
        spirv_offset = base + len(blobs)
        blobs += spirv_blob
        entries.append(struct.pack("<QIIII", int(shader, 16), dxil_offset, len(dxil_blob), spirv_offset, len(spirv_blob)))

    os.makedirs(os.path.dirname(os.path.abspath(arguments.output)), exist_ok=True)
    # Written beside and renamed over, so the running game never reads half a file.
    partial = arguments.output + ".partial"
    with open(partial, "wb") as handle:
        handle.write(b"KSHD" + struct.pack("<IIII", 3, len(shaders), len(meshes), model_type) + b"".join(entries) + struct.pack(f"<{len(meshes)}Q", *meshes) + blobs)
    os.replace(partial, arguments.output)
    print(f"{len(shaders)} shaders, {(base + len(blobs)) / 1e6:.1f} MB: {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
