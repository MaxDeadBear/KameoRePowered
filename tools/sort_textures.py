#!/usr/bin/env python3
"""Sort an export_textures.py export by where each texture is used and how much an upscale would gain.

    python tools/sort_textures.py out/build/release/textures/export

Adds to the export, as hard links so it takes no more disk space:

    sorted/by_priority/1_characters/   textures from model files: what the player looks at most
    sorted/by_priority/2_world/        large colour textures from levels
    sorted/by_priority/3_detail/       smaller colour textures
    sorted/by_priority/4_normal/       normal maps, including those stored as colour, which need their own treatment
    sorted/by_priority/5_atlas/        neon false-colour atlases, which hold data rather than art and are among the largest
    sorted/by_priority/6_mask/         greyscale, one- and two-channel textures
    sorted/by_priority/7_skip/         flat colours and tiny textures, which an upscale can't improve
    sorted/by_area/<area>/<priority>/  the same, in each place that uses them; shared/ holds those many places use
    sorted/sheets/                     contact sheets of each priority
    sorted/sorted.csv                  the manifest with priority, area and detail added

Level and model files are named by the ELF hash of the level's or monster's name, which is how an area gets its name
here. All DLC goes under DLC.
"""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import sys
from collections import Counter, defaultdict

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from export_textures import thumbnail, write_sheets  # noqa: E402

# Below this much brightness variation a texture is a flat colour, and an upscale changes nothing.
FLAT_DETAIL = 3.0
TINY_SIDE = 64
WORLD_SIDE = 256
# A texture in more places than this belongs to no one area.
SHARED_AREAS = 3
# Every name a game file is hashed from, read from the unpacked archives. Model files add a number: kameo_00.mdl.
LEVELS = (
    "ArmadilloCave", "CastleIntro", "ChamberOfPortraits", "DungeonOfEternalLife", "FairyCastleGrounds", "FairyCastleInterior",
    "FairyHut1", "FairyHut2", "FairyTowerRoom", "FairyTrainerHut", "Field", "FieldHutInterior1", "FieldHutInterior2",
    "FieldHutInterior3", "FieldHutInterior4", "FieldHutInterior5", "FieldHutInterior6", "FieldHutInterior7", "FieldHutInterior8",
    "ForestHutInterior", "ForestHutInterior2", "ForestHutInterior3", "ForestHutInterior5", "ForestTemple", "ForestTempleBossRoom",
    "ForgottenForest", "ForgottenForestCave", "ForgottenForestEntrance", "FrontEndLevel", "FrostyCave", "KnightBossRoom",
    "LevelRequiredObjects", "MountainFalls", "MountainFallsUpper", "MysticInterior", "OgresSwamp", "PlantTrainerHut", "PuddlesCave",
    "QuadapusCave", "ShadowFightLevel1", "SnowHutInterior2", "SnowHutInterior3", "SnowHutInterior4", "SnowTemple",
    "SnowTempleBossRoom", "SnowTopTribe", "ThornBossRoom", "ThornsPass", "TrainerHut2", "TrainerHut3", "TreasureCave",
    "WaterHutInterior1", "WaterHutInterior2", "WaterHutInterior3", "WaterHutInterior4", "WaterHutInterior5", "WaterTemple",
    "WaterTempleBossRoom", "WaterTempleInside",
)
MODELS = (
    "armadillo", "fire_ant", "firedragon", "flowerboxer", "ice_b_yeti", "kameo", "rubble", "snow_man", "spitter", "watercannon",
    "watercreature_small",
)
# False-colour atlases are pure green, blue, yellow and black, where painted art has shades between. Tuned on textures
# picked from the contact sheets: glows are mostly black, and lava is pure red and orange with no green or blue.
ATLAS_EXTREME = 0.30
ATLAS_COOL = 0.03
ATLAS_BLACK = 0.50
ATLAS_SIDE = 256
PRIORITIES = ("1_characters", "2_world", "3_detail", "4_normal", "5_atlas", "6_mask", "7_skip")


def link(source: str, destination: str) -> None:
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def measure(path: str) -> dict[str, float | bool]:
    """What a texture looks like, from a small copy, so it costs little."""
    with Image.open(path) as image:
        small = image.convert("RGB").resize((64, 64), Image.Resampling.BILINEAR)
        # Nearest, because blending would soften the pure colours an atlas is told by.
        sampled = np.asarray(image.convert("RGB").resize((256, 256), Image.Resampling.NEAREST), dtype=np.int32)
    rgb = np.asarray(small, dtype=np.float32)
    extreme = ((sampled < 48) | (sampled > 207)).all(axis=2)
    black = sampled.max(axis=2) < 32
    # A channel at zero as well, so white backgrounds don't count: pale art is near white, an atlas never is.
    cool = extreme & ~black & (sampled.min(axis=2) < 48) & ((sampled[:, :, 1] > 207) | (sampled[:, :, 2] > 207))
    red, green, blue = rgb[:, :, 0].mean(), rgb[:, :, 1].mean(), rgb[:, :, 2].mean()
    highest, lowest = rgb.max(axis=2), rgb.min(axis=2)
    middling = abs(red - 128) < 45 and abs(green - 128) < 45
    # Normal maps sit around one colour: blue with the direction in red and green, or pink with it in green and blue.
    blue_normal = blue > 200 and middling
    pink_normal = red > 215 and abs(green - 128) < 45 and abs(blue - 128) < 45
    return {
        "detail": float(rgb.mean(axis=2).std()),
        "normal": bool(blue_normal or pink_normal),
        "grey": float((highest - lowest).mean()) < 6.0,
        "atlas": bool(extreme.mean() >= ATLAS_EXTREME and cool.mean() >= ATLAS_COOL and black.mean() < ATLAS_BLACK),
    }


def elf_hash(name: str) -> int:
    """The hash the game names its files with; CurrentLevelToken in src/engine/collectibles.cpp."""
    value = 0
    for character in name.encode("ascii"):
        value = (value << 4) + character
        high = value & 0xF0000000
        if high:
            value ^= high >> 24
        value &= ~high & 0xFFFFFFFF
    return value


FILE_NAMES = {f"{elf_hash(name):08X}": name for name in LEVELS + MODELS}
# Each language folder's own front-end level. No string in the game hashes to this, so the name is ours.
FILE_NAMES["0582CB8C"] = "LanguageLevel"


def area_of(archive: str) -> str:
    """The level or monster an archive in a manifest belongs to."""
    if archive.startswith("dlc:"):
        return "DLC"
    stem = os.path.splitext(archive.rsplit("/", 1)[-1].split(":", 1)[-1])[0]
    return FILE_NAMES.get(stem.split("_")[0].upper(), stem)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("export", help="the folder export_textures.py wrote")
    parser.add_argument("--out", help="folder to sort into; it must be new or empty (default: sorted inside the export)")
    arguments = parser.parse_args()

    out = arguments.out or os.path.join(arguments.export, "sorted")
    if os.path.isdir(out) and os.listdir(out):
        parser.error(f"{out} is not empty")
    with open(os.path.join(arguments.export, "manifest.csv"), newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    print(f"{len(rows)} textures")

    thumbnails: dict[str, list[tuple[str, Image.Image]]] = defaultdict(list)
    tally: Counter = Counter()
    areas: Counter = Counter()
    for number, row in enumerate(rows, 1):
        width, height = int(row["width"]), int(row["height"])
        file_name = f"{row['hash']}_{width}x{height}_fmt{format_number(row)}.png"
        source = os.path.join(arguments.export, row["folder"], f"{width}x{height}", file_name)
        if not os.path.exists(source):
            matches = [f for f in os.listdir(os.path.dirname(source)) if f.startswith(row["hash"])] if os.path.isdir(os.path.dirname(source)) else []
            if not matches:
                tally["missing"] += 1
                continue
            source = os.path.join(os.path.dirname(source), matches[0])
            file_name = matches[0]

        look = measure(source)
        detail = look["detail"]
        archives = [a for a in row["sources"].split(";") if a]
        from_model = any(a.lower().endswith(".mdl") for a in archives) or any(a.startswith("dlc:") for a in archives)
        if row["folder"] == "normal":
            priority = "4_normal"
        elif row["folder"] == "mask":
            priority = "6_mask"
        elif max(width, height) < TINY_SIDE:
            priority = "7_skip"
        elif look["normal"]:
            priority = "4_normal"
        elif detail < FLAT_DETAIL:
            priority = "7_skip"
        elif look["grey"]:
            priority = "6_mask"
        elif look["atlas"] and max(width, height) >= ATLAS_SIDE:
            priority = "5_atlas"
        elif from_model:
            priority = "1_characters"
        elif max(width, height) >= WORLD_SIDE:
            priority = "2_world"
        else:
            priority = "3_detail"

        places = {area_of(a) for a in archives}
        places.update(level for level in row["levels_seen_in_dumps"].split(";") if level and level != "global")
        homes = sorted(places) if 0 < len(places) <= SHARED_AREAS else ["shared" if places else "unknown"]

        link(source, os.path.join(out, "by_priority", priority, file_name))
        for home in homes:
            link(source, os.path.join(out, "by_area", home, priority, file_name))
        with Image.open(source) as image:
            thumbnails[priority].append((row["hash"][:12], thumbnail(np.asarray(image.convert("RGBA")))))
        row.update(priority=priority, area=";".join(homes), detail=f"{detail:.1f}")
        tally[priority] += 1
        areas.update(homes)
        if number % 500 == 0:
            print(f"  {number}/{len(rows)}", flush=True)

    with open(os.path.join(out, "sorted.csv"), "w", newline="", encoding="utf-8") as handle:
        fields = ["priority", "area", "hash", "width", "height", "format", "detail", "folder", "used_by", "levels_seen_in_dumps", "sources"]
        writer = csv.DictWriter(handle, fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(sorted((r for r in rows if "priority" in r), key=lambda r: (r["priority"], r["area"], -int(r["width"]) * int(r["height"]))))

    pages = write_sheets(out, {k: thumbnails[k] for k in PRIORITIES if k in thumbnails})
    print("priorities", {k: tally[k] for k in PRIORITIES}, "missing", tally["missing"])
    print("areas", areas.most_common(40))
    print(f"{pages} contact sheets")
    return 0


FORMAT_NUMBERS = {"DXT1": 18, "DXT3": 19, "DXT5": 20, "DXN": 49, "8888": 6, "565": 4, "8": 2, "88": 10}


def format_number(row: dict) -> str:
    return str(FORMAT_NUMBERS.get(row["format"], row["format"].replace("format", "")))


if __name__ == "__main__":
    sys.exit(main())
