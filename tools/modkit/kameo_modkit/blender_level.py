"""The Blender level editor: a new level for the game made in a Blender scene, built into the game's Level Select.

  View3D > Sidebar > Kameo > Kameo Level
    New Level        a scene of its own on one of the bases (custom_level.BASES): the base's room materials, with
                     their textures, a floor and a player start
    Add              the player start, troll spawners (Water Temple), props (the base's own crates, urns, torches...)
                     and a troll area, the floor the trolls keep to
    Build Level      the scene through custom_level.install: the level's archive and its line in custom_levels.toml,
                     in the game's assets folder, so it shows in Level Select under Custom Levels
    Build and Play   the same, then the game, which goes on into the level once a save has loaded

What each object is, is its Kameo role (the Selected box). Meshes are Drawn and Solid unless set otherwise: Drawn Only
has no collision, Solid Only is an invisible wall. Faces draw with the base's material their Blender material was made
from (the "<base> nn" materials New Level adds); any other material draws as the floor's. Markers (the start, spawners
and props) are wire boxes at their size, standing on their origin and facing along -Y, Blender's front.

Blender to game: Z up to Y up with X mirrored, game = (-x, z, -y), one unit each; the mirror reverses the winding; a
game UV is (u, 1 - v); a game yaw is atan2 of the facing, 0 along game +Z (Blender -Y).
"""

from __future__ import annotations

import math
import os
import subprocess

import bpy
from bpy.props import EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty
from mathutils import Vector

from . import custom_level as C

ROLES = [
    ("SOLID", "Drawn and Solid", "Drawn in the level, and solid to walk on and against"),
    ("DRAWN", "Drawn Only", "Drawn, with nothing to stop anyone walking through it"),
    ("COLLISION", "Solid Only", "An invisible wall or floor"),
    ("AREA", "Troll Area", "The floor the trolls keep to, drawn from above as one flat shape (Water Temple)"),
    ("START", "Player Start", "Where Kameo starts, facing along its -Y"),
    ("SPAWNER", "Troll Spawner", "Makes trolls from the start of the level (Water Temple)"),
    ("PROP", "Prop", "One of the base level's objects: crates, urns, torches..."),
    ("IGNORE", "Not in the Level", "Left out: references, helpers"),
]
MARKERS = {"START", "SPAWNER", "PROP", "AREA"}
MARKER_ROLES = [r for r in ROLES if r[0] in MARKERS]
# A Blender size for each marker (width, depth, height): Kameo about 1.6 x 1.2 x 5.5, a troll a bit more.
MARKER_SIZES = {"START": (1.6, 1.2, 5.5), "SPAWNER": (3.0, 3.0, 6.5), "PROP": (2.0, 2.0, 2.0)}
DEFAULT_MATERIALS = {"hut": (6, 7), "temple": (14, 13)}   # floor, walls
SPAWN_LIFT = 0.5            # starts and spawners stand this far above their marker's origin, as the game's own do


def _assets() -> str:
    from .blender import _assets_folder

    return _assets_folder()


def _base_items(self, context):
    return [(b.key, b.label, b.description) for b in C.BASES.values()]


def _prop_items(self, context):
    types = context.scene.get("kameo_level_types") or []
    return [(t, t, "") for t in types] or [("", "(build the level's materials first)", "")]


def _level(context):
    return context.scene.kameo_level if context.scene.get("kameo_level_scene") else None


class KameoLevelSettings(bpy.types.PropertyGroup):
    name: StringProperty(name="Name", description="The level's name in the game: letters, digits and _, no spaces. Its file is named after it", default="MyLevel", maxlen=31)
    title: StringProperty(name="Title", description="What Level Select and the arrival caption show", default="My Level")
    base: EnumProperty(name="Base", description="The game level the new one is built over: its materials, props and enemies", items=_base_items)
    prop_type: EnumProperty(name="Prop", description="The base level's object to place", items=_prop_items)
    tile: FloatProperty(name="Tile", description="Game units per texture repeat", default=8.0, min=0.25)


class KameoObjectSettings(bpy.types.PropertyGroup):
    role: EnumProperty(name="Role", items=ROLES, default="SOLID")
    prop_type: StringProperty(name="Type", description="The base level's object type this prop is")
    max_alive: IntProperty(name="At Once", description="Trolls alive from this spawner at a time", default=1, min=1, max=10)
    max_total: IntProperty(name="In All", description="Trolls this spawner makes before it stops", default=5, min=1, max=100)
    interval: FloatProperty(name="Every", description="Seconds between trolls", default=5.0, min=0.5, unit="TIME_ABSOLUTE")


# --- the scene -------------------------------------------------------------------------------------------------

def _palette_folder(base: str) -> str:
    folder = os.path.join(bpy.utils.user_resource("DATAFILES", path="kameo_level", create=True), base)
    os.makedirs(folder, exist_ok=True)
    return folder


def _material_name(base: str, number: int) -> str:
    return f"{C.BASES[base].label} {number:02d}"


def load_materials(context, base: str) -> list[C.Material]:
    """A Blender material for each of the base's room materials, its colour texture packed in; the base's prop types
    kept on the scene for the Prop list."""
    assets = _assets()
    folder = _palette_folder(base)
    materials = C.palette(base, assets, folder)
    for m in materials:
        name = _material_name(base, m.number)
        material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
        material["kameo_slot"], material["kameo_base"] = m.number, base
        material.use_nodes = True
        nodes = material.node_tree.nodes
        shader = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
        image_node = next((n for n in nodes if n.type == "TEX_IMAGE"), None) or nodes.new("ShaderNodeTexImage")
        path = os.path.join(folder, f"material_{m.number}.png")
        image = bpy.data.images.get(f"kameo_{base}_{m.number}")
        if image is None:
            image = bpy.data.images.load(path)
            image.name = f"kameo_{base}_{m.number}"
        else:
            image.filepath = path
            image.reload()
        image.pack()
        image_node.image = image
        image_node.location = (-320, 300)
        if shader is not None:
            material.node_tree.links.new(image_node.outputs["Color"], shader.inputs["Base Color"])
            shader.inputs["Roughness"].default_value = 0.9
        material.diffuse_color = (*[c for c in image.pixels[:3]], 1.0) if len(image.pixels) >= 3 else (0.6, 0.6, 0.6, 1.0)
        material.use_fake_user = True
    context.scene["kameo_level_types"] = C.placeable_types(base, assets)
    return materials


def _collection(context) -> bpy.types.Collection:
    scene = context.scene
    collection = scene.collection.children.get("Kameo Level") or bpy.data.collections.get(f"Kameo Level {scene.name}")
    if collection is None:
        collection = bpy.data.collections.new(f"Kameo Level {scene.name}")
        scene.collection.children.link(collection)
    return collection


def _link(context, obj) -> None:
    _collection(context).objects.link(obj)
    for other in context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


def _box_mesh(name: str, size, arrow: bool) -> bpy.types.Mesh:
    """A box standing on its origin (wire display shows its edges), with an arrow on the ground along -Y."""
    w, d, h = size[0] / 2, size[1] / 2, size[2]
    verts = [(-w, -d, 0), (w, -d, 0), (w, d, 0), (-w, d, 0), (-w, -d, h), (w, -d, h), (w, d, h), (-w, d, h)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    if arrow:
        a = len(verts)
        verts += [(-w * 0.6, -d, 0.02), (w * 0.6, -d, 0.02), (0, -d - max(w, d) * 1.4, 0.02)]
        faces.append((a, a + 2, a + 1))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    return mesh


def add_marker(context, role: str, name: str, location=None, **settings) -> bpy.types.Object:
    obj = bpy.data.objects.new(name, _box_mesh(name, MARKER_SIZES[role], arrow=role in ("START", "SPAWNER")))
    obj.location = location if location is not None else context.scene.cursor.location
    obj.display_type = "WIRE"
    obj.show_name = True
    obj.hide_render = True
    obj.kameo.role = role
    for key, value in settings.items():
        setattr(obj.kameo, key, value)
    _link(context, obj)
    return obj


def project_uvs(obj, tile: float) -> None:
    """UVs projected from the world along each face's main axis, one repeat every `tile` units, as the game's own
    rooms are textured: textures keep their size on every face."""
    mesh = obj.data
    layer = mesh.uv_layers.active or mesh.uv_layers.new(name="UVMap")
    world = obj.matrix_world
    normal_matrix = world.to_3x3().inverted_safe().transposed()
    for poly in mesh.polygons:
        n = normal_matrix @ poly.normal
        axis = max(range(3), key=lambda i: abs(n[i]))
        u_axis, v_axis = [(1, 2), (0, 2), (0, 1)][axis]
        for loop in poly.loop_indices:
            p = world @ mesh.vertices[mesh.loops[loop].vertex_index].co
            layer.data[loop].uv = (p[u_axis] / tile, p[v_axis] / tile)


def _plane(context, name: str, size: float, material) -> bpy.types.Object:
    h = size / 2
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(-h, -h, 0), (h, -h, 0), (h, h, 0), (-h, h, 0)], [], [(0, 1, 2, 3)])
    mesh.update()
    if material is not None:
        mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    _link(context, obj)
    return obj


# --- scene to design -------------------------------------------------------------------------------------------

def _game(p) -> tuple[float, float, float]:
    return (-p[0], p[2], -p[1])


def _yaw(obj) -> float:
    f = obj.matrix_world.to_3x3() @ Vector((0.0, -1.0, 0.0))
    return math.degrees(math.atan2(-f.x, -f.y)) % 360.0


def _feet(obj) -> tuple[float, float, float]:
    x, y, z = _game(obj.matrix_world.translation)
    return (x, y + SPAWN_LIFT, z)


def _triangles(mesh):
    """The mesh's triangles, each with its three corners' normals (smooth, flat or custom, as Blender shades them)."""
    if bpy.app.version >= (4, 1, 0):             # corner normals are always there; 3.6 has the name, empty
        mesh.calc_loop_triangles()
        normals = mesh.corner_normals
        return [(tri, [tuple(normals[loop].vector) for loop in tri.loops]) for tri in mesh.loop_triangles]
    mesh.calc_normals_split()
    mesh.calc_loop_triangles()
    return [(tri, [tuple(n) for n in tri.split_normals]) for tri in mesh.loop_triangles]


def _surfaces(context, obj, default_slot: int, base: str, notes: list[str]) -> list[C.Surface]:
    depsgraph = context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        world = obj.matrix_world
        normal_matrix = world.to_3x3().inverted_safe().transposed()
        uv_layer = mesh.uv_layers.active
        slots = []
        for material in mesh.materials:
            if material is not None and material.get("kameo_base") == base and "kameo_slot" in material:
                slots.append(int(material["kameo_slot"]))
            else:
                if material is not None:
                    notes.append(f"{obj.name}: the material {material.name} isn't one of the {C.BASES[base].label}'s, so it draws as the floor's")
                slots.append(default_slot)
        if not slots:
            slots = [default_slot]
        role = obj.kameo.role
        by_slot: dict[int, C.Surface] = {}
        seen: dict[tuple, int] = {}
        for tri, tri_normals in _triangles(mesh):
            slot = slots[min(tri.material_index, len(slots) - 1)]
            surface = by_slot.get(slot)
            if surface is None:
                surface = by_slot[slot] = C.Surface(obj.name, slot, [], [], [] if uv_layer else None, [],
                                                    drawn=role in ("SOLID", "DRAWN"), solid=role in ("SOLID", "COLLISION"))
            corners = []
            for loop, corner_normal in zip(tri.loops, tri_normals):
                vertex = mesh.loops[loop].vertex_index
                uv = tuple(uv_layer.data[loop].uv) if uv_layer else (0.0, 0.0)
                normal = (normal_matrix @ Vector(corner_normal)).normalized()
                key = (slot, vertex, round(uv[0], 5), round(uv[1], 5), round(normal.x, 3), round(normal.y, 3), round(normal.z, 3))
                index = seen.get(key)
                if index is None:
                    index = seen[key] = len(surface.positions)
                    surface.positions.append(_game(world @ mesh.vertices[vertex].co))
                    surface.normals.append(_game(normal))
                    if uv_layer:
                        surface.uvs.append((uv[0], 1.0 - uv[1]))
                corners.append(index)
            # The mirror reverses the winding.
            surface.triangles.append((corners[0], corners[2], corners[1]))
        return list(by_slot.values())
    finally:
        evaluated.to_mesh_clear()


def _area_outline(context, obj) -> list[tuple[float, float]]:
    """The troll area's outer edge: its boundary edges in order, seen from above, the longest loop if there are
    several."""
    mesh = obj.data
    world = obj.matrix_world
    count: dict[tuple[int, int], int] = {}
    for poly in mesh.polygons:
        for key in poly.edge_keys:
            count[key] = count.get(key, 0) + 1
    edges = [k for k, n in count.items() if n == 1]
    neighbours: dict[int, list[int]] = {}
    for a, b in edges:
        neighbours.setdefault(a, []).append(b)
        neighbours.setdefault(b, []).append(a)
    loops, used = [], set()
    for start in neighbours:
        if start in used:
            continue
        loop, previous, at = [start], None, start
        used.add(start)
        while True:
            following = [n for n in neighbours[at] if n != previous and n not in used]
            if not following:
                break
            previous, at = at, following[0]
            used.add(at)
            loop.append(at)
        loops.append(loop)
    if not loops:
        return []
    loop = max(loops, key=len)
    points = [_game(world @ mesh.vertices[i].co) for i in loop]
    return [(p[0], p[2]) for p in points]


def design_of(context, notes: list[str]) -> C.Design:
    settings = context.scene.kameo_level
    base = settings.base
    default_slot = DEFAULT_MATERIALS[base][0]
    objects = [o for o in context.scene.objects if o.visible_get() or o.kameo.role in MARKERS]
    starts = [o for o in objects if o.kameo.role == "START"]
    if not starts:
        raise C.DesignError("The level needs a Player Start: Add > Player Start.")
    if len(starts) > 1:
        notes.append(f"{len(starts)} player starts: the level starts at {starts[0].name}")
    surfaces: list[C.Surface] = []
    for obj in objects:
        if obj.type == "MESH" and obj.kameo.role in ("SOLID", "DRAWN", "COLLISION"):
            surfaces += _surfaces(context, obj, default_slot, base, notes)
    spawners = [C.Placed(_feet(o), _yaw(o), "NormalTroll",
                         {"max_alive": o.kameo.max_alive, "max_total": o.kameo.max_total, "interval": o.kameo.interval})
                for o in objects if o.kameo.role == "SPAWNER"]
    props = [C.Placed(_game(o.matrix_world.translation), _yaw(o), o.kameo.prop_type)
             for o in objects if o.kameo.role == "PROP" and o.kameo.prop_type]
    areas = [o for o in objects if o.kameo.role == "AREA" and o.type == "MESH"]
    troll_area = _area_outline(context, areas[0]) if areas else None
    return C.Design(settings.name, settings.title, base, surfaces, C.Placed(_feet(starts[0]), _yaw(starts[0])),
                    spawners, props, troll_area)


# --- operators -------------------------------------------------------------------------------------------------

class KAMEO_OT_level_new(bpy.types.Operator):
    """Start a new level in a scene of its own, on one of the game's levels"""
    bl_idname = "kameo.level_new"
    bl_label = "New Kameo Level"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="Name", default="MyLevel", maxlen=31, description="Letters, digits and _, no spaces")
    title: StringProperty(name="Title", default="My Level")
    base: EnumProperty(name="Base", items=_base_items)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=360)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "name")
        layout.prop(self, "title")
        layout.prop(self, "base")
        layout.label(text=C.BASES[self.base].description, icon="INFO")

    def execute(self, context):
        if not _assets():
            self.report({"ERROR"}, "Set the game folder in the add-on's preferences first")
            return {"CANCELLED"}
        problems = C.check_name(self.name)
        if problems:
            self.report({"ERROR"}, problems[0])
            return {"CANCELLED"}
        scene = bpy.data.scenes.new(f"Kameo Level {self.name}")
        context.window.scene = scene
        scene["kameo_level_scene"] = True
        scene.kameo_level.name, scene.kameo_level.title, scene.kameo_level.base = self.name, self.title, self.base
        scene.unit_settings.system = "NONE"
        context.window.cursor_set("WAIT")
        try:
            load_materials(context, self.base)
        except Exception as error:  # a base the game folder doesn't have, or can't read
            self.report({"ERROR"}, f"Couldn't read the {C.BASES[self.base].label}: {error}")
            return {"CANCELLED"}
        finally:
            context.window.cursor_set("DEFAULT")
        floor = _plane(context, "Floor", 80.0, bpy.data.materials.get(_material_name(self.base, DEFAULT_MATERIALS[self.base][0])))
        project_uvs(floor, scene.kameo_level.tile)
        add_marker(context, "START", "Player Start", location=(0.0, 20.0, 0.0))
        for area in context.screen.areas:
            if area.type == "VIEW_3D":
                area.spaces.active.shading.type = "MATERIAL"
                area.spaces.active.clip_end = max(area.spaces.active.clip_end, 2000.0)
        self.report({"INFO"}, f"New level {self.name}: build it with Build Level when it's ready")
        return {"FINISHED"}


class KAMEO_OT_level_materials(bpy.types.Operator):
    """Read the base level's materials and props again (after a game update, or into a scene made elsewhere)"""
    bl_idname = "kameo.level_materials"
    bl_label = "Reload Materials"

    def execute(self, context):
        context.window.cursor_set("WAIT")
        try:
            count = len(load_materials(context, context.scene.kameo_level.base))
        finally:
            context.window.cursor_set("DEFAULT")
        self.report({"INFO"}, f"{count} materials")
        return {"FINISHED"}


class KAMEO_OT_level_add(bpy.types.Operator):
    """Add one of the level's markers at the 3D cursor"""
    bl_idname = "kameo.level_add"
    bl_label = "Add"
    bl_options = {"REGISTER", "UNDO"}

    role: EnumProperty(items=MARKER_ROLES)

    def execute(self, context):
        settings = context.scene.kameo_level
        if self.role == "START":
            existing = next((o for o in context.scene.objects if o.kameo.role == "START"), None)
            if existing is not None:
                existing.location = context.scene.cursor.location
                self.report({"INFO"}, "Moved the player start to the 3D cursor")
                return {"FINISHED"}
            add_marker(context, "START", "Player Start")
        elif self.role == "SPAWNER":
            if not C.BASES[settings.base].trolls:
                self.report({"ERROR"}, f"The {C.BASES[settings.base].label} has no trolls; spawners need the Water Temple")
                return {"CANCELLED"}
            add_marker(context, "SPAWNER", "Troll Spawner")
        elif self.role == "PROP":
            if not settings.prop_type:
                self.report({"ERROR"}, "Choose a prop first")
                return {"CANCELLED"}
            add_marker(context, "PROP", settings.prop_type, prop_type=settings.prop_type)
        else:
            obj = _plane(context, "Troll Area", 60.0, None)
            obj.location = context.scene.cursor.location
            obj.display_type = "WIRE"
            obj.hide_render = True
            obj.kameo.role = "AREA"
        return {"FINISHED"}


class KAMEO_OT_level_tile(bpy.types.Operator):
    """Texture the selected meshes from the world, one repeat every Tile units, so textures keep their size on every face"""
    bl_idname = "kameo.level_tile"
    bl_label = "Tile Textures"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        meshes = [o for o in context.selected_objects if o.type == "MESH" and o.kameo.role in ("SOLID", "DRAWN")]
        if not meshes:
            self.report({"ERROR"}, "Select the meshes to texture")
            return {"CANCELLED"}
        mode = context.object.mode if context.object else "OBJECT"
        if mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        for obj in meshes:
            project_uvs(obj, context.scene.kameo_level.tile)
        if mode != "OBJECT":
            bpy.ops.object.mode_set(mode=mode)
        return {"FINISHED"}


def _build(operator, context) -> bool:
    notes: list[str] = []
    if context.object and context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    assets = _assets()
    if not assets:
        operator.report({"ERROR"}, "Set the game folder in the add-on's preferences first")
        return False
    context.window.cursor_set("WAIT")
    try:
        design = design_of(context, notes)
        path, built = C.install(design, assets)
    except C.DesignError as error:
        operator.report({"ERROR"}, str(error))
        return False
    finally:
        context.window.cursor_set("DEFAULT")
    for note in notes + [n for n in built if n.startswith("warning")]:
        operator.report({"WARNING"}, note)
    print("Kameo level:", path, *built, sep="\n  ")
    operator.report({"INFO"}, f"Built {design.title}: Level Select > Custom Levels (the game reads new levels when it starts)")
    return True


class KAMEO_OT_level_build(bpy.types.Operator):
    """Write the level into the game, listed in Level Select under Custom Levels"""
    bl_idname = "kameo.level_build"
    bl_label = "Build Level"

    def execute(self, context):
        return {"FINISHED"} if _build(self, context) else {"CANCELLED"}


class KAMEO_OT_level_play(bpy.types.Operator):
    """Build the level, start the game and go into the level once a save has loaded"""
    bl_idname = "kameo.level_play"
    bl_label = "Build and Play"

    def execute(self, context):
        from .blender_textures import _game_folder, _game_running

        if _game_running():
            self.report({"ERROR"}, "Close the game first: it lists custom levels when it starts (or build, then pick it in Level Select)")
            return {"CANCELLED"}
        if not _build(self, context):
            return {"CANCELLED"}
        game = _game_folder()
        launcher = os.path.join(game, "KameoLauncher.exe")
        if not os.path.isfile(launcher):
            self.report({"ERROR"}, "KameoLauncher.exe is not in the game folder; set it in the add-on's preferences")
            return {"CANCELLED"}
        subprocess.Popen([launcher, "--play", "--start-level", context.scene.kameo_level.name], cwd=game,
                         creationflags=0x08000000 if os.name == "nt" else 0)
        self.report({"INFO"}, "Kameo is starting: load a save and the level follows")
        return {"FINISHED"}


class KAMEO_OT_level_remove(bpy.types.Operator):
    """Take the level out of the game (the scene stays)"""
    bl_idname = "kameo.level_remove"
    bl_label = "Remove from Game"

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        if C.uninstall(context.scene.kameo_level.name, _assets()):
            self.report({"INFO"}, "Removed from the game")
        else:
            self.report({"INFO"}, "It wasn't in the game")
        return {"FINISHED"}


# --- the panel -------------------------------------------------------------------------------------------------

class KAMEO_PT_level(bpy.types.Panel):
    bl_label = "Kameo Level"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Kameo"

    def draw(self, context):
        layout = self.layout
        settings = _level(context)
        if settings is None:
            layout.label(text="Make a new level for the game:")
            layout.operator(KAMEO_OT_level_new.bl_idname, text="New Level", icon="WORLD")
            return
        base = C.BASES[settings.base]
        col = layout.column(align=True)
        col.prop(settings, "name")
        col.prop(settings, "title")
        layout.label(text=f"On the {base.label}", icon="WORLD")

        box = layout.box()
        box.label(text="Add at the 3D cursor:")
        row = box.row(align=True)
        row.operator(KAMEO_OT_level_add.bl_idname, text="Player Start", icon="ARMATURE_DATA").role = "START"
        if base.trolls:
            row.operator(KAMEO_OT_level_add.bl_idname, text="Troll Spawner", icon="GHOST_ENABLED").role = "SPAWNER"
        row = box.row(align=True)
        row.prop(settings, "prop_type", text="")
        row.operator(KAMEO_OT_level_add.bl_idname, text="Prop", icon="MESH_CUBE").role = "PROP"
        if base.trolls:
            box.operator(KAMEO_OT_level_add.bl_idname, text="Troll Area", icon="MESH_GRID").role = "AREA"

        obj = context.object
        if obj is not None:
            box = layout.box()
            box.label(text=obj.name, icon="OBJECT_DATA")
            box.prop(obj.kameo, "role")
            if obj.kameo.role == "SPAWNER":
                col = box.column(align=True)
                col.prop(obj.kameo, "max_alive")
                col.prop(obj.kameo, "max_total")
                col.prop(obj.kameo, "interval")
            elif obj.kameo.role == "PROP":
                box.prop(obj.kameo, "prop_type")
            elif obj.kameo.role in ("SOLID", "DRAWN"):
                row = box.row(align=True)
                row.operator(KAMEO_OT_level_tile.bl_idname, icon="UV")
                row.prop(settings, "tile")

        box = layout.box()
        box.label(text=f"Materials: the \"{base.label} nn\" ones", icon="MATERIAL")
        box.operator(KAMEO_OT_level_materials.bl_idname, icon="FILE_REFRESH")

        col = layout.column(align=True)
        col.scale_y = 1.3
        col.operator(KAMEO_OT_level_build.bl_idname, icon="EXPORT")
        col.operator(KAMEO_OT_level_play.bl_idname, icon="PLAY")
        layout.operator(KAMEO_OT_level_remove.bl_idname, icon="TRASH")


CLASSES = (KameoLevelSettings, KameoObjectSettings, KAMEO_OT_level_new, KAMEO_OT_level_materials, KAMEO_OT_level_add,
           KAMEO_OT_level_tile, KAMEO_OT_level_build, KAMEO_OT_level_play, KAMEO_OT_level_remove, KAMEO_PT_level)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.kameo_level = PointerProperty(type=KameoLevelSettings)
    bpy.types.Object.kameo = PointerProperty(type=KameoObjectSettings)


def unregister():
    del bpy.types.Object.kameo
    del bpy.types.Scene.kameo_level
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
