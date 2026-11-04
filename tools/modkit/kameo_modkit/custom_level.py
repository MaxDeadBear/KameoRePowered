"""Builds a new level from a design: its own surfaces (drawn, solid or both), a player start, troll spawners and props,
written over a copy of one of the game's levels, which lends the level its materials, its object types and its code.

A design is what the Blender level editor (blender_level.py) makes of a scene, or a JSON file of the same shape:
  {"name": "MyLevel", "title": "My Level", "base": "temple",
   "surfaces": [{"object": "Floor", "material": 14, "drawn": true, "solid": true,
                 "positions": [[x, y, z], ...], "triangles": [[a, b, c], ...], "uvs": [[u, v], ...]}],
   "start": {"position": [x, y, z], "yaw": 0},
   "spawners": [{"position": [x, y, z], "yaw": 0, "baddie": "NormalTroll", "max_alive": 1, "max_total": 5,
                 "interval": 5.0}],
   "props": [{"type": "LargeCrateSmash", "position": [x, y, z], "yaw": 0}],
   "troll_area": [[x, z], ...]}                     (optional; made from the floor when left out)
Everything is in game space: Y up, game units, triangles wound counter-clockwise seen from their front, yaw in
degrees (0 faces +Z). Materials are the base's background slots (palette()).

What the copy keeps and loses:
  - The level object is renamed, so the copy loads under its own name (level.rename_level).
  - The room collision becomes the solid surfaces (hits.replace_mesh), surface type 0, the base's ordinary floor.
  - The room's background becomes the drawn surfaces, one push buffer per material (level_geometry.replace_background);
    the base's own room draws are emptied. Lighting is the base's baked lightmap, sampled evenly per face direction.
  - Every placed object goes but the "empty" record: the base's first two setup-0 spawn points become players 1 and 2
    at the start, and spawners and props are copies of the base's own instances of their type, created at start.
  - Trigger rules that change level, warp the player, play a film or switch objects are switched off (rule +5 = 0),
    so nothing leads back into the base's rooms; music and events stay.
  - A base with trolls gets one troll zone (troll_area, or the floor) and a walking grid over it (network.py):
    trolls have no wall collision and stay inside their zone.

Usage: python -m kameo_modkit.custom_level build design.json <game assets folder>   (writes the level and lists it)
       python -m kameo_modkit.custom_level palette <base> <game assets folder> <out folder>
       python -m kameo_modkit.custom_level bases
"""

from __future__ import annotations

import copy
import json
import math
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field

import numpy as np

from . import hits as H
from . import level as L
from . import network as N
from . import xmlblock as X
from .caff import Caff
from .graph import Ref
from .level_geometry import Part, _read_background, replace_background
from .material import read_material, texture_palette
from .texture import read_texture, to_pixels, write_png


@dataclass(frozen=True)
class Base:
    key: str
    level: str             # the level object's name in the base archive
    label: str
    description: str
    trolls: bool           # defines NormalTroll and BaddieGenerator, and has troll zones
    players: tuple[str, str]  # its setup-0 spawn points for players 1 and 2
    room_mesh_at: int = 0x78  # the room collision's single kd-tree mesh record

    @property
    def file(self) -> str:
        return L.level_file_name(self.level)


BASES = {
    "hut": Base("hut", "FieldHutInterior2", "Hut", "Wood, plaster and thatch; no enemies. Small and quick to load.",
                False, ("PlrSetupPoint0", "PlrSetupPoint1")),
    "temple": Base("temple", "WaterTempleInside", "Water Temple", "Carved stone, tiles and lava; trolls.",
                   True, ("PlrSetupPoint0", "PlrSetupPoint4")),
}

# Object types a design can't place: spawn points are the start's, and these need set-up a copy doesn't have.
NOT_PLACEABLE = {"", "PlrSetupPoint", "BaddieGenerator", "cameraObject", "aperture", "soundEmitter", "SkyCloudEmitter",
                 "TribeHelpPage", "CastleIntroFlag", "WaterTempleDoor2", "WaterTempleScreen", "FireHeadSwitch"}
# What a troll spawner can make in the temple, and the AI it runs.
BADDIES = {"NormalTroll": "GenericAggressive.dll"}
# Enemies placed on their own, which a copy can't start as the base does (they wait on its triggers and routes).
ENEMIES = {"NormalTroll", "ArcherTroll", "DiverTroll", "FireTroll", "NormalPlantTroll", "NormalWaterTroll",
           "WarriorSoldier", "TentacleTrap"}
# Trigger ops that lead out of the level or name objects the copy no longer has.
SWITCHED_OFF = {"levelChange", "warpPlayer", "playFmv", "playMovie", "activateObject", "deactivateObject"}
SPAWN_SPACING = 4.0
TROLL_MARGIN = 2.5        # about a troll's radius: how far the troll zone stays inside the floor's edge
LIST_FILE = "custom_levels.toml"


class DesignError(Exception):
    pass


@dataclass
class Surface:
    object: str
    material: int
    positions: list
    triangles: list
    uvs: list | None = None
    normals: list | None = None
    drawn: bool = True
    solid: bool = True


@dataclass
class Placed:
    position: tuple[float, float, float]
    yaw: float = 0.0
    type: str = ""
    settings: dict = field(default_factory=dict)


@dataclass
class Design:
    name: str
    title: str
    base: str
    surfaces: list[Surface]
    start: Placed
    spawners: list[Placed] = field(default_factory=list)
    props: list[Placed] = field(default_factory=list)
    troll_area: list[tuple[float, float]] | None = None

    @classmethod
    def from_dict(cls, data: dict) -> "Design":
        def placed(d: dict, kind_key: str = "type") -> Placed:
            settings = {k: v for k, v in d.items() if k not in ("position", "yaw", kind_key)}
            return Placed(tuple(map(float, d["position"])), float(d.get("yaw", 0.0)), d.get(kind_key, ""), settings)

        return cls(name=data["name"], title=data.get("title") or data["name"], base=data.get("base", "hut"),
                   surfaces=[Surface(**{k: v for k, v in s.items() if k in Surface.__dataclass_fields__})
                             for s in data.get("surfaces", [])],
                   start=placed(data["start"]),
                   spawners=[placed(s, "baddie") for s in data.get("spawners", [])],
                   props=[placed(p) for p in data.get("props", [])],
                   troll_area=[tuple(map(float, p)) for p in data["troll_area"]] if data.get("troll_area") else None)


# ------------------------------------------------------------------------------------------------------- checks

def check_name(name: str) -> list[str]:
    problems = []
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,30}", name or ""):
        problems.append("A level name is 1 to 31 letters, digits or _, starting with a letter (no spaces).")
    return problems


def listed_levels(assets: str) -> list[dict]:
    """custom_levels.toml's entries, in order, as dicts of name, title and start."""
    path = os.path.join(assets, LIST_FILE)
    if not os.path.isfile(path):
        return []
    levels, current = [], None
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line == "[[level]]":
            current = {}
            levels.append(current)
        elif current is not None and "=" in line and not line.startswith("#"):
            key, value = (part.strip() for part in line.split("=", 1))
            value = value.split(" #")[0].strip()
            current[key] = json.loads(value) if value.startswith('"') else int(value)
    return levels


def _write_list(assets: str, levels: list[dict]) -> None:
    lines = ["# Custom levels: each one's archive is the ELF hash of its name in hex, .lvl, in this folder."]
    for level in levels:
        lines += ["[[level]]", f"name = {json.dumps(level['name'])}  # {L.level_file_name(level['name'])}",
                  f"title = {json.dumps(level.get('title') or level['name'], ensure_ascii=False)}",
                  f"start = {int(level.get('start', 0))}", ""]
    with open(os.path.join(assets, LIST_FILE), "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines))


def check_target(assets: str, name: str) -> list[str]:
    """Whether `name` can be written into `assets`: never over an archive that isn't a custom level's (every one of
    the game's own levels is there)."""
    problems = check_name(name)
    if problems:
        return problems
    file = L.level_file_name(name)
    ours = any(level["name"] == name for level in listed_levels(assets))
    if os.path.exists(os.path.join(assets, file)) and not ours:
        problems.append(f"The name {name!r} is taken: its file {file} is one of the game's own. Choose another name.")
    clash = next((level["name"] for level in listed_levels(assets)
                  if level["name"] != name and L.level_file_name(level["name"]) == file), None)
    if clash:
        problems.append(f"{name!r} and the custom level {clash!r} would share the file {file}. Choose another name.")
    return problems


# -------------------------------------------------------------------------------------------------- the palette

@dataclass
class Material:
    number: int
    texture: str          # the colour texture's file name in the base ("" when it has none)
    simple: bool          # one colour texture over the lightmap: draws on new geometry as it does in the base
    triangles: int        # how much of the base's room uses it


def _colour_sampler(material, palette):
    usable = [s for s in material.samplers if not s.dynamic and not s.lightmap and s.palette_index < len(palette)
              and palette[s.palette_index] is not None]
    return next((s for s in usable if s.channel == "colour"), None) or next(
        (s for s in usable if "colour" in s.file_node), None) or next(iter(usable), None)


def palette(base: str, assets: str, out: str | None = None) -> list[Material]:
    """The base's room materials, with each one's colour texture written to `out` as material_<n>.png."""
    archive = Caff.load(os.path.join(assets, BASES[base].file))
    background = _read_background(archive, 0)
    resolver = background.resolver
    table = texture_palette(resolver, Ref(background.entry, 0))
    materials = []
    if out:
        os.makedirs(out, exist_ok=True)
    for slot in background.slots:
        material = read_material(resolver, slot.material)
        sampler = _colour_sampler(material, table)
        if sampler is None:
            continue
        texture = read_texture(resolver, table[sampler.palette_index])
        if texture.format_name == "DXN":
            continue  # a normal map alone: nothing to show
        if out:
            write_png(os.path.join(out, f"material_{slot.number}.png"), to_pixels(resolver, texture))
        simple = slot.flags == 0x4701 and any("ambient0" in t for t in slot.textures)
        materials.append(Material(slot.number, sampler.file_node.replace("colour", "", 1), simple, slot.triangles))
    return materials


def placeable_types(base: str, assets: str) -> list[str]:
    archive = Caff.load(os.path.join(assets, BASES[base].file))
    _, objects = X.load_level_objects(archive)
    return sorted({o.type_name for o in objects.objects} - NOT_PLACEABLE - ENEMIES)


# ------------------------------------------------------------------------------------------------- the troll zone

def _joined(surfaces: list[Surface], want) -> tuple[np.ndarray, np.ndarray, list[str]]:
    """Every surface `want` keeps as one vertex array and an (n, 4) triangle array whose last column is the surface's
    number in the returned name list."""
    vertices, triangles, names = [], [], []
    count = 0
    for s in surfaces:
        if not want(s) or not s.triangles:
            continue
        v = np.asarray(s.positions, dtype=np.float64).reshape(-1, 3)
        t = np.asarray(s.triangles, dtype=np.int64).reshape(-1, 3)
        if t.max() >= len(v) or t.min() < 0:
            raise DesignError(f"{s.object}: a triangle names a vertex it doesn't have")
        vertices.append(v)
        triangles.append(np.column_stack([t + count, np.full(len(t), len(names))]))
        names.append(s.object)
        count += len(v)
    if not vertices:
        return np.zeros((0, 3)), np.zeros((0, 4), dtype=np.int64), names
    return np.concatenate(vertices), np.concatenate(triangles), names


def _pieces(vertices: np.ndarray, triangles: np.ndarray) -> list[np.ndarray]:
    """The triangles as connected pieces (sharing a corner, after welding equal positions): each piece's triangle
    numbers. One object holding several boxes is several pieces."""
    _, weld = np.unique(np.round(vertices, 3), axis=0, return_inverse=True)
    weld = weld.reshape(-1)
    parent = list(range(int(weld.max()) + 1 if len(weld) else 0))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in triangles[:, :3]:
        a, b, c = (find(int(weld[i])) for i in t)
        parent[b] = a
        parent[find(c)] = a
    roots = np.array([find(int(weld[t[0]])) for t in triangles[:, :3]])
    return [np.flatnonzero(roots == r) for r in np.unique(roots)]


def floor_area(surfaces: list[Surface], margin: float = TROLL_MARGIN):
    """The troll zone a design gets when it draws none: the rectangle of the main floor (the height most of the
    walkable ground is at), `margin` in from its edge, with each solid object standing on it inside that rectangle as
    an obstacle circle the trolls walk round. Returns (outline, obstacles, floor height)."""
    vertices, triangles, names = _joined(surfaces, lambda s: s.solid)
    if not len(triangles):
        raise DesignError("The level has nothing solid to stand on.")
    corners = vertices[triangles[:, :3]]
    normal = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    area = np.linalg.norm(normal, axis=1)
    up = normal[:, 1] / np.maximum(area, 1e-12)
    walkable = (up > 0.7) & (area > 1e-9)
    if not walkable.any():
        raise DesignError("The level has no ground flat enough to walk on.")
    heights = corners[walkable][:, :, 1].mean(axis=1)
    # The floor height: the half-unit band holding the most walkable area.
    bands = np.round(heights * 2) / 2
    weights = {}
    for band, a in zip(bands, area[walkable]):
        weights[band] = weights.get(band, 0.0) + a
    floor_y = max(weights, key=weights.get)
    on_floor = walkable.copy()
    on_floor[walkable] = np.abs(heights - floor_y) <= 0.5
    flat = corners[on_floor].reshape(-1, 3)
    x0, z0 = flat[:, 0].min() + margin, flat[:, 2].min() + margin
    x1, z1 = flat[:, 0].max() - margin, flat[:, 2].max() - margin
    if x1 - x0 < 2 or z1 - z0 < 2:
        raise DesignError("The floor is too small for trolls to walk on.")
    outline = [(float(x0), float(z0)), (float(x1), float(z0)), (float(x1), float(z1)), (float(x0), float(z1))]
    obstacles = []
    for piece in _pieces(vertices, triangles):
        mine = corners[piece].reshape(-1, 3)
        lo, hi = mine.min(axis=0), mine.max(axis=0)
        if hi[1] < floor_y + 1.0 or lo[1] > floor_y + 6.0:
            continue  # a step anyone can climb, or overhead
        if lo[0] <= x0 or hi[0] >= x1 or lo[2] <= z0 or hi[2] >= z1:
            continue  # reaches the zone's edge: the floor itself, a wall, or something against one
        radius = math.hypot(hi[0] - lo[0], hi[2] - lo[2]) / 2 + margin
        if math.pi * radius * radius > 0.25 * (x1 - x0) * (z1 - z0):
            continue  # too big to walk round: draw a troll area instead
        obstacles.append((float(lo[0] + hi[0]) / 2, float(lo[2] + hi[2]) / 2, float(radius), float(hi[1] - floor_y)))
    return outline, obstacles, float(floor_y)


# -------------------------------------------------------------------------------------------------------- build

def _object_from(templates: dict[str, X.LevelObject], type_name: str) -> X.LevelObject:
    if type_name not in templates:
        raise DesignError(f"The base level has no {type_name} to copy.")
    return templates[type_name]


def _place(obj: X.LevelObject, placed: Placed) -> None:
    obj.position = placed.position
    obj.rotation = (float(placed.yaw) % 360.0, 0.0, 0.0)
    obj.group, obj.event, obj.event2, obj.create_at_start = -1, -1, -1, 1


def build(design: Design, assets: str, out: str) -> list[str]:
    """Writes the level `design` describes to `out`, from the base archive in `assets`. Returns notes."""
    if design.base not in BASES:
        raise DesignError(f"No base level {design.base!r}; choose one of {', '.join(BASES)}.")
    base = BASES[design.base]
    problems = check_name(design.name)
    if problems:
        raise DesignError(problems[0])
    if design.spawners and not base.trolls:
        raise DesignError(f"The {base.label} base has no trolls; build on the Water Temple to use spawners.")
    notes: list[str] = []
    archive = Caff.load(os.path.join(assets, base.file))
    replaced: dict[int, bytes] = {}

    # Name.
    replaced[L.database_entry(archive).index] = L.rename_level(archive, base.level, design.name)

    # Collision.
    vertices, triangles, _ = _joined(design.surfaces, lambda s: s.solid)
    if not len(triangles):
        raise DesignError("Nothing in the level is solid: mark the floor Solid, or nobody has anything to stand on.")
    solid = triangles.copy()
    solid[:, 3] = 0
    level_entry = next(e for e in archive.entries if e.read(archive).startswith(b"kameolevel"))
    room = archive.pointers_from(level_entry)[0x34][0]
    pointers = next(r.offsets for r in archive.relocations if r.source == room.index and r.target == room.index)
    try:
        data, all_pointers = H.replace_mesh(room.read(archive), set(pointers), base.room_mesh_at, vertices, solid)
    except ValueError as error:
        raise DesignError(f"Collision: {error}") from error
    H.set_self_pointers(archive, room, all_pointers)
    replaced[room.index] = data
    notes.append(f"collision: {len(solid)} triangles")

    # Objects: only what the design places, each a copy of the base's own.
    objects_entry, objects = X.load_level_objects(archive)
    templates: dict[str, X.LevelObject] = {}
    for obj in objects.objects:
        templates.setdefault(obj.type_name, copy.deepcopy(obj))
    players = [copy.deepcopy(objects.get(n)) for n in base.players]
    objects.objects = [o for o in objects.objects if o.name == "empty"]
    start = design.start
    facing = math.radians(start.yaw)
    for number, player in enumerate(players):
        # Player 2 beside player 1, to their right.
        offset = SPAWN_SPACING * number
        player.position = (start.position[0] - offset * math.cos(facing), start.position[1],
                           start.position[2] + offset * math.sin(facing))
        player.rotation = (float(start.yaw) % 360.0, 0.0, 0.0)
        objects.objects.append(player)
    for number, spawner in enumerate(design.spawners):
        baddie = spawner.type or "NormalTroll"
        if baddie not in BADDIES or baddie not in templates:
            raise DesignError(f"A spawner can make {', '.join(BADDIES)}, not {baddie}.")
        g = objects.add(_object_from(templates, "BaddieGenerator"), f"Spawner{number}")
        _place(g, spawner)
        g.health_scale = 1.0
        s = spawner.settings
        for name, value in (("baddie", baddie), ("ai_dll", BADDIES[baddie]), ("node_name", ""),
                            ("max_alive", int(s.get("max_alive", 1))), ("max_total", int(s.get("max_total", 5))),
                            ("start_count", min(1, int(s.get("max_alive", 1)))),
                            ("spawn_interval", float(s.get("interval", 5.0))), ("spawn_style", 6), ("stop_event", -1)):
            g.params.set(name, value)
    counts: dict[str, int] = {}
    for prop in design.props:
        if prop.type in NOT_PLACEABLE or prop.type in ENEMIES:
            raise DesignError(f"{prop.type} can't be placed as a prop.")
        counts[prop.type] = counts.get(prop.type, 0) + 1
        name = f"{prop.type[:28]}{counts[prop.type]}"
        _place(objects.add(_object_from(templates, prop.type), name), prop)
    objects.objects.sort(key=lambda o: X.elf_hash_latin1(o.name))
    replaced[objects_entry.index] = objects.to_bytes()
    notes.append(f"objects: 2 player starts, {len(design.spawners)} spawners, {len(design.props)} props")

    # Triggers: nothing leads out, nothing names an object the copy dropped.
    trig_entry = X.triggers_entry(archive)
    if trig_entry is not None:
        block = X.XmlBlock.parse(trig_entry.read(archive))
        script = block.find("level")
        if script is not None:
            rule, off = None, 0
            data = bytearray(script.data)
            for offset, op, fields in X.decode_trigger_script(bytes(data)):
                if op == "rule":
                    rule = offset
                elif op in SWITCHED_OFF and rule is not None and data[rule + 5]:
                    data[rule + 5] = 0
                    off += 1
            script.data = bytes(data)
            replaced[trig_entry.index] = block.to_bytes()
            notes.append(f"triggers: {off} rules switched off")

    # Trolls' zone and walking grid.
    if base.trolls:
        if design.troll_area:
            outline, obstacles = list(design.troll_area), []
            floor_y = float(np.median([p[1] for p in [design.start.position, *[s.position for s in design.spawners]]]))
        else:
            outline, obstacles, floor_y = floor_area(design.surfaces)
        problems = N.check_outline(outline)
        if problems:
            raise DesignError("Troll area: " + problems[0])
        zones_entry, old_zones = N.load_zones(archive)
        net_entry, old_net = N.load_network(archive)
        zones = N.arena_zones(old_zones, outline, obstacles, name="Arena", y=floor_y, height=20.0)
        network = N.grid_network(old_net, zones, spacing=12.0, y=floor_y + 0.5)
        replaced[zones_entry.index] = zones.to_bytes()
        replaced[net_entry.index] = network.to_bytes()
        notes.append(f"troll zone: {len(outline)} corners, {len(obstacles)} obstacles, {len(network.nodes)} walking nodes")
        for spawner in design.spawners:
            x, _, z = spawner.position
            if not zones.zones[0].contains(x, z):
                notes.append(f"warning: a spawner at ({x:.1f}, {z:.1f}) is outside the troll area; its trolls will walk into it")

    # The room's background: one part per material.
    parts: dict[int, Part] = {}
    for s in design.surfaces:
        if not s.drawn or not s.triangles:
            continue
        part = parts.setdefault(int(s.material), Part(int(s.material), [], [], [], []))
        first = len(part.positions)
        part.positions += [tuple(map(float, p)) for p in s.positions]
        part.triangles += [tuple(int(i) + first for i in t) for t in s.triangles]
        uvs = s.uvs or [(p[0] / 8.0, -p[2] / 8.0) for p in s.positions]
        part.uvs += [tuple(map(float, uv)) for uv in uvs]
        if s.normals:
            part.normals += [tuple(map(float, n)) for n in s.normals]
        else:
            part.normals = None
    if not parts:
        raise DesignError("Nothing in the level is drawn: it would be invisible.")
    for part in parts.values():
        if len(part.positions) > 0xFFFF:
            raise DesignError(f"Material {part.material} has {len(part.positions)} vertices; one material holds 65535.")
        if part.normals is not None and len(part.normals) != len(part.positions):
            part.normals = None
    step = tempfile.NamedTemporaryFile(suffix=".lvl", delete=False)
    step.close()
    try:
        open(step.name, "wb").write(archive.write(replaced))
        notes += replace_background(step.name, out, list(parts.values()))
    finally:
        os.remove(step.name)
    return notes


def install(design: Design, assets: str) -> tuple[str, list[str]]:
    """Builds `design` into the game's assets folder and lists it in custom_levels.toml, so it shows in Level Select.
    Returns the archive's path and the build's notes."""
    problems = check_target(assets, design.name)
    if problems:
        raise DesignError(problems[0])
    path = os.path.join(assets, L.level_file_name(design.name))
    partial = path + ".partial"
    try:
        notes = build(design, assets, partial)
        os.replace(partial, path)
    finally:
        if os.path.exists(partial):
            os.remove(partial)
    levels = [lv for lv in listed_levels(assets) if lv["name"] != design.name]
    levels.append({"name": design.name, "title": design.title, "start": 0})
    _write_list(assets, levels)
    return path, notes


def uninstall(name: str, assets: str) -> bool:
    levels = listed_levels(assets)
    if not any(lv["name"] == name for lv in levels):
        return False
    _write_list(assets, [lv for lv in levels if lv["name"] != name])
    path = os.path.join(assets, L.level_file_name(name))
    if os.path.exists(path):
        os.remove(path)
    return True


def main(argv: list[str]) -> int:
    if argv[:1] == ["bases"]:
        for base in BASES.values():
            print(f"{base.key:8} {base.label}: {base.description} ({base.file})")
        return 0
    if argv[:1] == ["palette"] and len(argv) == 4:
        for m in palette(argv[1], argv[2], argv[3]):
            print(f"{m.number:3}  {'simple ' if m.simple else 'layered'}  {m.triangles:6} triangles  {m.texture}")
        print("types:", ", ".join(placeable_types(argv[1], argv[2])))
        return 0
    if argv[:1] == ["build"] and len(argv) == 3:
        design = Design.from_dict(json.load(open(argv[1], encoding="utf-8")))
        try:
            path, notes = install(design, argv[2])
        except DesignError as error:
            print("error:", error)
            return 1
        for note in notes:
            print(note)
        print("wrote", path)
        return 0
    print(__doc__.split("Usage:")[1])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
