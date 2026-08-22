#!/usr/bin/env python3
"""Build the Kameo shader cache with XenosRecomp.

Runs the recompiler as ONE PROCESS PER SHADER, with retries, then merges the
per-shader artefacts into a single shader_cache.cpp.

Why not just point XenosRecomp at a directory? Its batch mode corrupts the heap
once it has driven DXC a few hundred times in one process. The failure is
nondeterministic, worsens with volume, and survives both serialisation and
per-shader DxcCompiler construction (see src/gfx/SHADERS.md for the full
elimination table). Isolating each shader in its own process reduces the failure
rate to something a retry loop absorbs cleanly.

Two sources, and for Kameo you want BOTH:

  --image       the DECOMPRESSED image, not assets/default.xex (the shipping XEX
                is compressed and a raw scan finds zero containers). Dump it out
                of a running build -- see SHADERS.md. This yields the engine's
                ~145 built-in shaders.

  --containers  a directory of container .bin files captured at runtime. Most of
                Kameo's shaders are decompressed out of packed game data into the
                heap at load time and never appear in the XEX at all, so the
                image alone gives roughly a 50-hit / 1326-miss cache. The
                creation hook in kameo_gfx_hooks.cpp writes them to
                shader_dump/<hash>.bin as it sees them.

Coverage note: the captured set only contains shaders the play session actually
reached, so it grows as more of the game is visited. Duplicates between the two
sources are harmless -- they hash identically and get deduplicated.

Usage:
  python scripts/build_shader_cache.py \
      --exe        thirdparty/XenosRecomp/build/XenosRecomp/Release/XenosRecomp.exe \
      --image      guest_image.bin \
      --containers out/build/native/shader_dump \
      --out        src/gfx/shader_cache.cpp
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC_MASK = 0xFFFFFF00
MAGIC = 0x102A0E00  # Kameo container version 0x0E (Unleashed is 0x11)
HEADER = 24  # six dwords


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def find_containers(data):
    """Scan for shader containers using the validated v0x0E criteria."""
    out = []
    i = 0
    n = len(data)
    while i + HEADER < n:
        if (be32(data, i) & MAGIC_MASK) != MAGIC:
            i += 4
            continue
        virtual_size = be32(data, i + 4)
        physical_size = be32(data, i + 8)
        shader_offset = be32(data, i + 20)
        total = virtual_size + physical_size
        ok = (
            0 < shader_offset < virtual_size
            and (shader_offset & 3) == 0
            and 0 < total <= n - i
        )
        if ok:
            out.append((i, total))
            i += total
        else:
            i += 4
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", default=None, help="decompressed guest image (built-in shaders)")
    # Scanning the image is NOT sufficient for Kameo: most shaders are
    # decompressed out of packed game data into the heap at load time, so they
    # never appear in the XEX. The runtime creation hook captures those to
    # shader_dump/<hash>.bin -- point --containers at that directory. See the
    # correction section in src/gfx/SHADERS.md.
    ap.add_argument("--containers", default=None,
                    help="directory of captured container .bin files")
    ap.add_argument("--out", required=True)
    ap.add_argument("--include", default=None,
                    help="shader_common.h (default: alongside --exe's source tree)")
    ap.add_argument("--retries", type=int, default=8)
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    include = args.include
    if include is None:
        root = os.path.dirname(exe)
        for _ in range(6):
            cand = os.path.join(root, "XenosRecomp", "shader_common.h")
            if os.path.isfile(cand):
                include = cand
                break
            root = os.path.dirname(root)
    if not include or not os.path.isfile(include):
        sys.exit("could not locate shader_common.h; pass --include")
    include = os.path.abspath(include)

    if not args.image and not args.containers:
        sys.exit("pass --image, --containers, or both")

    # (label, bytes) from either source. Duplicates across the two are harmless:
    # they hash identically and the dedup below collapses them.
    sources = []
    if args.image:
        with open(args.image, "rb") as f:
            image = f.read()
        found = find_containers(image)
        print(f"found {len(found)} shader containers in {args.image}")
        for off, total in found:
            sources.append((f"image@0x{off:X}", image[off:off + total]))

    if args.containers:
        names = sorted(n for n in os.listdir(args.containers) if n.endswith(".bin"))
        print(f"found {len(names)} captured containers in {args.containers}")
        for n in names:
            with open(os.path.join(args.containers, n), "rb") as f:
                sources.append((n, f.read()))

    print(f"{len(sources)} containers total")

    work = tempfile.mkdtemp(prefix="kameo_shaders_")
    entries = []
    failed = []
    salvaged_dxil = []
    try:
        for idx, (label, blob) in enumerate(sources):
            stem = os.path.join(work, f"{idx:04d}")
            binp = stem + ".bin"
            with open(binp, "wb") as f:
                f.write(blob)

            for attempt in range(args.retries):
                proc = subprocess.run(
                    [exe, binp, stem, include, "--blobs"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                if proc.returncode == 0 and os.path.isfile(stem + ".meta"):
                    break
            else:
                # A SPIR-V failure is NOT a reason to drop the shader. XenosRecomp
                # returns non-zero and writes no .meta when the Vulkan backend
                # rejects a shader ("partial explicit stage input location
                # assignment via vk::location(X) unsupported"), but it has already
                # written a perfectly good .dxil by then -- and the D3D12 path is
                # the only one this title uses. Dropping the whole entry is what
                # left a family of skinned-character vertex shaders permanently
                # out of the cache, so their draws were discarded at runtime and
                # parts of character models never rendered. Salvage the DXIL.
                #
                # The hash normally comes from .meta; for a captured container the
                # file name IS the hash, which covers every failure seen so far.
                # An image-scanned container has no such name, so it still has to
                # be dropped -- reported separately rather than silently.
                dxil_path = stem + ".dxil"
                stem_hash = os.path.splitext(os.path.basename(label))[0]
                salvaged = False
                if os.path.isfile(dxil_path) and os.path.getsize(dxil_path) > 0:
                    try:
                        h = int(stem_hash, 16)
                    except ValueError:
                        h = None
                    if h is not None:
                        entries.append({
                            "hash": h,
                            "dxil": dxil_path,
                            "dxil_size": os.path.getsize(dxil_path),
                            "spirv": stem + ".spirv",
                            "spirv_size": 0,
                            "spec": 0,
                        })
                        salvaged_dxil.append((idx, label))
                        salvaged = True
                if not salvaged:
                    failed.append((idx, label, proc.stderr.decode(errors="replace")[:200]))
                continue

            with open(stem + ".meta") as f:
                h, dxil_size, spirv_size, spec = f.read().split()
            entries.append({
                "hash": int(h, 16),
                "dxil": stem + ".dxil",
                "dxil_size": int(dxil_size),
                "spirv": stem + ".spirv",
                "spirv_size": int(spirv_size),
                "spec": int(spec),
            })

            if (idx + 1) % 20 == 0:
                print(f"  {idx + 1}/{len(sources)}")

        print(f"compiled {len(entries)}, failed {len(failed)}, "
              f"salvaged dxil-only {len(salvaged_dxil)}")
        for idx, label in salvaged_dxil:
            print(f"  SALVAGED (spirv failed, dxil kept) #{idx} {label}")
        for idx, label, err in failed:
            print(f"  FAILED #{idx} {label} {err.strip()}")

        # Deduplicate by hash, preserving first occurrence.
        seen = set()
        unique = []
        for e in entries:
            if e["hash"] in seen:
                continue
            seen.add(e["hash"])
            unique.append(e)
        unique.sort(key=lambda e: e["hash"])
        print(f"{len(unique)} unique shaders after dedup")

        dxil_blob = bytearray()
        spirv_blob = bytearray()
        rows = []
        for e in unique:
            d_off = len(dxil_blob)
            if e["dxil_size"]:
                with open(e["dxil"], "rb") as f:
                    dxil_blob += f.read()
            s_off = len(spirv_blob)
            if e["spirv_size"]:
                with open(e["spirv"], "rb") as f:
                    spirv_blob += f.read()
            rows.append((e["hash"], d_off, e["dxil_size"], s_off, e["spirv_size"], e["spec"]))

        try:
            import zstandard as zstd
            comp = zstd.ZstdCompressor(level=19)
            dxil_c, spirv_c = comp.compress(bytes(dxil_blob)), comp.compress(bytes(spirv_blob))
            compressed = True
        except ImportError:
            dxil_c, spirv_c = bytes(dxil_blob), bytes(spirv_blob)
            compressed = False
            print("NOTE: zstandard module not installed; emitting UNCOMPRESSED caches.")

        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        with open(args.out, "w", newline="\n") as f:
            f.write('#include "shader_cache.h"\n\n')
            f.write("// Generated by scripts/build_shader_cache.py -- do not edit.\n")
            f.write(f"// {len(unique)} unique shaders; caches are "
                    f"{'zstd-compressed' if compressed else 'UNCOMPRESSED'}.\n\n")
            f.write("ShaderCacheEntry g_shaderCacheEntries[] = {\n")
            for r in rows:
                f.write("\t{{ 0x{:X}, {}, {}, {}, {}, {} }},\n".format(*r))
            f.write("};\n\n")

            def emit(name, data):
                f.write(f"const uint8_t {name}[] = {{")
                f.write(",".join(str(b) for b in data))
                f.write("};\n")

            emit("g_compressedDxilCache", dxil_c)
            f.write(f"const size_t g_dxilCacheCompressedSize = {len(dxil_c)};\n")
            f.write(f"const size_t g_dxilCacheDecompressedSize = {len(dxil_blob)};\n")
            emit("g_compressedSpirvCache", spirv_c)
            f.write(f"const size_t g_spirvCacheCompressedSize = {len(spirv_c)};\n")
            f.write(f"const size_t g_spirvCacheDecompressedSize = {len(spirv_blob)};\n")
            f.write(f"const size_t g_shaderCacheEntryCount = {len(unique)};\n")

        print(f"wrote {args.out} "
              f"(dxil {len(dxil_blob)} -> {len(dxil_c)}, spirv {len(spirv_blob)} -> {len(spirv_c)})")
        return 1 if failed else 0
    finally:
        if args.keep:
            print(f"scratch kept at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
