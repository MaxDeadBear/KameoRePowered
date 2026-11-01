#!/usr/bin/env python3
"""Export every texture in the game and its DLC, without playing, ready to sort and upscale.

Reads the game's CAFF archives (.lvl, .mdl) directly, so it covers areas nobody has reached, and adds the textures
from a dump folder that the archives don't explain. Each PNG is named by the hash the game gives that texture, so an
edited copy works as it is in textures/replace/.

    python tools/export_textures.py --assets out/build/release/assets --out out/build/release/textures/export \\
        --dump out/build/release/textures/dump --dlc "%USERPROFILE%/Documents/kameorepowered"

The output holds:

    colour/        plain colour textures, the main work of an upscale
    colour_alpha/  colour with transparency, whose alpha should be scaled on its own
    normal/        normal maps (red and green hold a direction), which colour upscalers damage
    mask/          one- and two-channel textures
    dump_only/     textures a dump holds and the archives don't
    sheets/        contact sheets of each folder, to sort through at a glance
    manifest.csv   every texture: hash, size, format, folder, where it comes from, and whether a dump saw it

Needs NumPy, xxhash and Pillow.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

import numpy as np
import xxhash
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "modkit"))

from kameo_modkit.caff import Caff  # noqa: E402
from kameo_modkit.graph import Ref, Resolver  # noqa: E402
from kameo_modkit import texture as tx  # noqa: E402

# The fetch constant's swizzle goes into the game's hash but is not in the archive. Each format always draws with the
# same one, except a few 8888 textures, which the dump's hashes tell apart.
SWIZZLE_DEFAULT = 0x688
SWIZZLE_BY_FORMAT = {10: 0x200, 2: 0xA00}
SWIZZLE_ALTERNATES = {6: [0xA0A]}

THUMB = 192
SHEET_COLUMNS = 10
SHEET_ROWS = 7
LABEL = 16
DUMP_NAME = re.compile(r"([0-9A-F]{16})_(\d+)x(\d+)_fmt(\d+)")


@dataclass
class Exported:
    hash: int
    width: int
    height: int
    format: int
    folder: str
    sources: set[str] = field(default_factory=set)


def game_hash(data: bytes, width: int, height: int, gpu_format: int, swizzle: int) -> int:
    """TextureContentHash in src/gpu/textures/texture_pack.cpp."""
    return xxhash.xxh3_64_intdigest(data, seed=(width << 40) ^ (height << 20) ^ (gpu_format << 12) ^ swizzle)


def read_dumps(folder: str | None) -> dict[int, tuple[str, str]]:
    """Every dumped hash: its level folder and file."""
    dumps: dict[int, tuple[str, str]] = {}
    if not folder:
        return dumps
    for directory, _, files in os.walk(folder):
        for name in files:
            match = DUMP_NAME.match(name)
            if match:
                dumps.setdefault(int(match[1], 16), (os.path.relpath(directory, folder).split(os.sep)[0], os.path.join(directory, name)))
    return dumps


def find_archives(root: str) -> list[str]:
    found = []
    for directory, _, files in os.walk(root):
        for name in files:
            if name.lower().endswith((".lvl", ".mdl")) or "." not in name:
                path = os.path.join(directory, name)
                with open(path, "rb") as handle:
                    if handle.read(4) == b"CAFF":
                        found.append(path)
    return sorted(found)


def folder_of(gpu_format_name: str, pixels: np.ndarray) -> str:
    if gpu_format_name == "DXN":
        return "normal"
    if gpu_format_name in ("8", "88"):
        return "mask"
    return "colour_alpha" if (pixels[:, :, 3] != 255).any() else "colour"


def thumbnail(pixels: np.ndarray) -> Image.Image:
    image = Image.fromarray(pixels, "RGBA")
    image.thumbnail((THUMB, THUMB), Image.Resampling.BILINEAR)
    backed = Image.new("RGB", image.size, (40, 32, 64))
    backed.paste(image, mask=image.getchannel("A"))
    return backed


def write_sheets(out: str, thumbnails: dict[str, list[tuple[str, Image.Image]]]) -> int:
    folder = os.path.join(out, "sheets")
    os.makedirs(folder, exist_ok=True)
    pages = 0
    per_page = SHEET_COLUMNS * SHEET_ROWS
    for category, items in thumbnails.items():
        for page, start in enumerate(range(0, len(items), per_page), 1):
            sheet = Image.new("RGB", (SHEET_COLUMNS * (THUMB + 8), SHEET_ROWS * (THUMB + LABEL + 8)), (16, 12, 32))
            draw = ImageDraw.Draw(sheet)
            for i, (label, thumb) in enumerate(items[start:start + per_page]):
                x = (i % SHEET_COLUMNS) * (THUMB + 8) + 4
                y = (i // SHEET_COLUMNS) * (THUMB + LABEL + 8) + 4
                sheet.paste(thumb, (x, y))
                draw.text((x, y + THUMB + 2), label, fill=(240, 200, 120))
            sheet.save(os.path.join(folder, f"{category}_{page:03}.jpg"), quality=88)
            pages += 1
    return pages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--assets", required=True, help="the game's assets folder")
    parser.add_argument("--out", required=True, help="folder to write into; it must be new or empty")
    parser.add_argument("--dump", help="a textures/dump folder, for level names and textures the archives don't hold")
    parser.add_argument("--dlc", help="the folder DLC is installed under, usually Documents/kameorepowered")
    parser.add_argument("--threads", type=int, default=max(2, (os.cpu_count() or 4) - 2))
    arguments = parser.parse_args()

    if os.path.isdir(arguments.out) and os.listdir(arguments.out):
        parser.error(f"{arguments.out} is not empty")
    dumps = read_dumps(arguments.dump)
    roots = [("game", arguments.assets)]
    if arguments.dlc:
        roots.append(("dlc", os.path.expandvars(os.path.join(arguments.dlc, "0000000000000000", "4D5307D2", "00000002"))))

    exported: dict[int, Exported] = {}
    thumbnails: dict[str, list[tuple[int, str, Image.Image]]] = defaultdict(list)
    skipped: dict[str, int] = defaultdict(int)

    def export(resolver: Resolver, texture, data: bytes, key: int) -> None:
        pixels = tx.to_pixels(resolver, texture)
        hashes = [game_hash(data, texture.width, texture.height, texture.gpu_format, SWIZZLE_BY_FORMAT.get(texture.gpu_format, SWIZZLE_DEFAULT))]
        hashes += [game_hash(data, texture.width, texture.height, texture.gpu_format, s) for s in SWIZZLE_ALTERNATES.get(texture.gpu_format, [])]
        final = next((h for h in hashes if h in dumps), hashes[0])
        category = folder_of(texture.format_name, pixels)
        name = f"{final:016X}_{texture.width}x{texture.height}_fmt{texture.gpu_format}.png"
        tx.write_png(os.path.join(arguments.out, category, f"{texture.width}x{texture.height}", name), pixels)
        entry = exported[key]
        entry.hash, entry.folder = final, category
        thumbnails[category].append((texture.width * texture.height, f"{final:016X}"[:12], thumbnail(pixels)))

    seen: set[int] = set()
    os.makedirs(arguments.out, exist_ok=True)
    made_folders: set[str] = set()
    with ThreadPoolExecutor(arguments.threads) as pool:
        for group, root in roots:
            archives = find_archives(root)
            print(f"{group}: {len(archives)} archives")
            for number, path in enumerate(archives, 1):
                label = f"{group}:{os.path.relpath(path, root).replace(os.sep, '/')}"
                try:
                    archive = Caff.load(path)
                except ValueError as error:
                    print(f"  skipped {label}: {error}")
                    continue
                resolver = Resolver(archive)
                jobs = []
                for entry in archive.entries:
                    if resolver.type_name(entry) != "texture":
                        continue
                    texture = tx.read_texture(resolver, Ref(entry, 0))
                    if texture.kind != tx.KIND_2D:
                        skipped[f"kind {texture.kind}"] += 1
                        continue
                    if texture.gpu_format not in tx.FORMATS or texture.data is None:
                        skipped[f"format {texture.gpu_format}"] += 1
                        continue
                    data = tx.base_level_bytes(resolver, texture)
                    key = xxhash.xxh3_64_intdigest(data, seed=(texture.width << 40) ^ (texture.height << 20) ^ (texture.gpu_format << 12))
                    if key in seen:
                        exported[key].sources.add(label)
                        continue
                    seen.add(key)
                    exported[key] = Exported(0, texture.width, texture.height, texture.gpu_format, "", {label})
                    for category in ("colour", "colour_alpha", "normal", "mask"):
                        folder = os.path.join(arguments.out, category, f"{texture.width}x{texture.height}")
                        if folder not in made_folders:
                            os.makedirs(folder, exist_ok=True)
                            made_folders.add(folder)
                    jobs.append(pool.submit(export, resolver, texture, data, key))
                for job in jobs:
                    job.result()
                print(f"  {number}/{len(archives)} {label}: {len(exported)} unique so far", flush=True)

    # Size folders no texture landed in.
    for category in ("colour", "colour_alpha", "normal", "mask"):
        for name in os.listdir(os.path.join(arguments.out, category)):
            folder = os.path.join(arguments.out, category, name)
            if not os.listdir(folder):
                os.rmdir(folder)

    by_hash = {item.hash: item for item in exported.values()}
    dump_only = 0
    for value, (level, path) in dumps.items():
        if value in by_hash:
            by_hash[value].sources.add(f"level:{level}")
            continue
        match = DUMP_NAME.match(os.path.basename(path))
        folder = os.path.join(arguments.out, "dump_only", f"{match[2]}x{match[3]}")
        os.makedirs(folder, exist_ok=True)
        shutil.copy2(path, folder)
        by_hash[value] = Exported(value, int(match[2]), int(match[3]), int(match[4]), "dump_only", {f"level:{level}"})
        with Image.open(path) as image:
            thumbnails["dump_only"].append((by_hash[value].width * by_hash[value].height, f"{value:016X}"[:12], thumbnail(np.asarray(image.convert("RGBA")))))
        dump_only += 1

    with open(os.path.join(arguments.out, "manifest.csv"), "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["hash", "width", "height", "format", "folder", "used_by", "levels_seen_in_dumps", "sources"])
        for item in sorted(by_hash.values(), key=lambda i: (i.folder, -i.width * i.height, i.hash)):
            levels = sorted(s[6:] for s in item.sources if s.startswith("level:"))
            archives = sorted(s for s in item.sources if not s.startswith("level:"))
            writer.writerow([f"{item.hash:016X}", item.width, item.height, tx.FORMATS.get(item.format, (f"format{item.format}",))[0], item.folder, len(archives), ";".join(levels), ";".join(archives[:8])])

    pages = write_sheets(arguments.out, {k: [(label, thumb) for _, label, thumb in sorted(v, key=lambda t: -t[0])] for k, v in thumbnails.items()})
    counts: dict[str, int] = defaultdict(int)
    for item in by_hash.values():
        counts[item.folder] += 1
    print(f"exported {len(by_hash)} textures: {dict(counts)}; {dump_only} from dumps only; {pages} contact sheets; skipped {dict(skipped)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
