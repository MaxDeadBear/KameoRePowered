"""New costumes: a copy of one of the game's skin models that the game lists as a costume of its own.

The game (src/engine/dlc/mod_costumes.cpp) gathers <game>/costumes/<model type>_m<id>.mdl into a package shaped like
downloadable content, named from costumes/costumes.toml, so the copy is chosen on the Bonus page like any costume and the
model it was made from is never touched. The id is six hex digits of the costume name's hash, as the game keeps shader
costumes; the suffix ("m" and the id) is seven characters, the most a save holds.

The copy's textures would hash as the original's do, so an image painted for one would show on both. Making a costume
changes one texel in the base level of each texture it was copied with, a corner nobody sees, which gives it hashes of its
own: textures/replace images painted for the costume then show on it alone.
"""

from __future__ import annotations

import os
import re

from .caff import Caff
from .graph import Ref, Resolver
from .texture import FORMATS, KIND_2D, TILE_BLOCKS, game_hashes, read_texture, tiled_offset_2d

NAMES = "costumes.toml"
FILE = re.compile(r"([0-9A-Fa-f]{8})_m([0-9A-Fa-f]{6})\.mdl$")


def costume_id(name: str) -> int:
    """CostumeId in src/engine/dlc/dlc_skin_names.cpp: FNV-1a over the name's UTF-16 units, 24 bits, never 0."""
    value = 2166136261
    units = name.encode("utf-16-le")
    for i in range(0, len(units), 2):
        value = ((value ^ (units[i] | units[i + 1] << 8)) * 16777619) & 0xFFFFFFFF
    return (value & 0xFFFFFF) or 1


def costume_file(base_file: str, name: str) -> str:
    model = os.path.basename(base_file)[:8].upper()
    return f"{model}_m{costume_id(name):06X}.mdl"


def is_costume(path: str) -> bool:
    return FILE.match(os.path.basename(path)) is not None


def own_textures(data: bytes, key: int) -> bytes:
    """The archive with one texel changed in every texture's base level, picked by `key`, so its textures hash apart
    from the archive it was copied from (and from other costumes copied from it)."""
    archive = Caff.parse(data)
    resolver = Resolver(archive)
    changed: dict[int, bytearray] = {}
    done: set[tuple[int, int]] = set()
    for entry in archive.entries:
        if resolver.type_name(entry) != "texture":
            continue
        texture = read_texture(resolver, Ref(entry, 0))
        if texture.kind != KIND_2D or texture.gpu_format not in FORMATS or texture.data is None:
            continue
        where = (texture.data.entry.index, texture.data.offset)
        if where in done:
            continue
        done.add(where)
        _, block, block_bytes, _ = FORMATS[texture.gpu_format]
        blocks_x, blocks_y = (texture.width + block - 1) // block, (texture.height + block - 1) // block
        k = key % (blocks_x * blocks_y)
        x, y = k % blocks_x, k // blocks_x
        if texture.format & 0x100:
            at = tiled_offset_2d(x, y, blocks_x, block_bytes.bit_length() - 1) * block_bytes
        else:
            at = (y * ((blocks_x + TILE_BLOCKS - 1) // TILE_BLOCKS) * TILE_BLOCKS + x) * block_bytes
        body = changed.setdefault(texture.data.entry.index, bytearray(texture.data.entry.read(archive)))
        # A block's last byte holds texel indices (DXT, DXN) or a low colour bit (uncompressed): one texel moves to its
        # neighbouring shade.
        body[texture.data.offset + at + block_bytes - 1] ^= 1
    return archive.write({index: bytes(body) for index, body in changed.items()})


def texture_hashes(path: str) -> dict[int, list[str]]:
    """Each texture asset's game hashes, by entry index, as the manifest (kameo_textures.json) records them."""
    archive = Caff.load(path)
    resolver = Resolver(archive)
    out = {}
    for entry in archive.entries:
        if resolver.type_name(entry) != "texture":
            continue
        texture = read_texture(resolver, Ref(entry, 0))
        if texture.kind == KIND_2D and texture.gpu_format in FORMATS and texture.data is not None:
            out[entry.index] = [f"{h:016X}" for h in game_hashes(resolver, texture)]
    return out


def read_names(folder: str) -> dict[str, str]:
    """costumes.toml: file name -> costume name."""
    names = {}
    try:
        with open(os.path.join(folder, NAMES), encoding="utf-8") as handle:
            for line in handle:
                match = re.match(r'\s*"([^"]+)"\s*=\s*"((?:[^"\\]|\\.)*)"', line)
                if match:
                    names[match.group(1)] = re.sub(r"\\(.)", r"\1", match.group(2))
    except OSError:
        pass
    return names


def write_names(folder: str, names: dict[str, str]) -> None:
    def quoted(text: str) -> str:
        return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

    lines = ["# Costumes made with the Blender add-on: file = the name the game shows."]
    lines += [f"{quoted(file)} = {quoted(name)}" for file, name in names.items()]
    with open(os.path.join(folder, NAMES), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def write_costume(game: str, file: str, name: str, data: bytes) -> str:
    """Saves the costume's model and its name; the path written."""
    folder = os.path.join(game, "costumes")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, file)
    with open(path + ".tmp", "wb") as handle:
        handle.write(data)
    os.replace(path + ".tmp", path)
    names = read_names(folder)
    names[file] = name
    write_names(folder, names)
    return path
