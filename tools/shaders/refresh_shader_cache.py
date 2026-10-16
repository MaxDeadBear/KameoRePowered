#!/usr/bin/env python3
"""Merge every shader_dump into the canonical one, then rebuild the cache.

The runtime writes shader_dump/<hash>.bin relative to its WORKING DIRECTORY, so
running the game from out/build/relwithdebinfo and building the cache from the repo root
silently maintains two separate libraries. That happened: the two had diverged
to 38 / 415 containers held only on one side, and rebuilding from either alone
threw away the other's coverage.

Containers are content-hashed, so merging is a plain union with no conflicts.
Canonical home is <repo>/shader_dump.

  python tools/shaders/refresh_shader_cache.py [--dry-run]
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CANON = os.path.join(ROOT, "shader_dump")
BUILD_DIRS = [os.path.join(ROOT, "out", "build", preset) for preset in ("relwithdebinfo", "release")]
SEARCH = [os.path.join(d, "shader_dump") for d in BUILD_DIRS]
EXE = os.path.join(ROOT, "thirdparty", "XenosRecomp", "build", "XenosRecomp", "Release",
                   "XenosRecomp.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    os.makedirs(CANON, exist_ok=True)
    have = set(os.listdir(CANON))
    added = 0
    for d in SEARCH:
        if not os.path.isdir(d):
            continue
        new = [f for f in os.listdir(d) if f.endswith(".bin") and f not in have]
        print("%-52s %4d new" % (os.path.relpath(d, ROOT), len(new)))
        for f in new:
            if not args.dry_run:
                shutil.copy2(os.path.join(d, f), os.path.join(CANON, f))
            have.add(f)
            added += 1
    print("canonical shader_dump now holds %d containers (+%d)" % (len(have), added))
    if args.dry_run:
        return 0

    image_cand = os.path.join(ROOT, "guest_image.bin")
    if not os.path.isfile(image_cand):
        for cand in [os.path.join(d, "guest_image.bin") for d in BUILD_DIRS]:
            if os.path.isfile(cand):
                image_cand = cand
                break

    cmd = [sys.executable, os.path.join(ROOT, "tools", "shaders", "build_shader_cache.py"),
           "--exe", EXE,
           "--containers", CANON,
           "--out", os.path.join(ROOT, "res", "shader_cache", "shader_cache.cpp")]
    if os.path.isfile(image_cand):
        cmd.extend(["--image", image_cand])
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
