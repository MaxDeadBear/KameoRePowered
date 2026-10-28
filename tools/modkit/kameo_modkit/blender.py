"""The Blender add-on: Kameo characters in, mods out, without the naming rules the command line asks for.

  File > Import > Kameo Character   a model from the game's assets, by name, textured and rigged (images packed into
                                     the .blend, the source file remembered on the scene)
  View3D > Sidebar > Kameo          Attach to Character: the selected model becomes a new part of the character, on
                                     the bone nearest it (or one chosen), as a new draw of its own with its own
                                     textures; and the export button
  File > Export > Kameo Mod         the scene as a .kmod in the launcher's mods folder, switched on there, with any
                                     problem explained

Underneath it is importer.py's rules: a material named graph<g>_mesh<m>_draw<d> is one of the game's draws, a copy of
one (.001) a new draw beside it, bones are the game's joint_<n>. Attaching is the part that knows them: it joins the
part into the mesh of a draw it can copy (one whose bone palette already follows that bone, a plain textured one where
it can), renames the part's materials as copies of that draw's, gives every vertex to the bone and puts its UVs and
colours under the names the mesh uses.
"""

from __future__ import annotations

import json
import math
import os
import re
import tempfile

import bmesh
import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy_extras.io_utils import ExportHelper
from mathutils import Vector

from .caff import Caff
from .graph import Ref, Resolver, rendergraph_roots
from .material import read_material, texture_palette
from .texture import read_texture
from .mesh import BLENDINDICES, TEXCOORD, collect_meshes
from .skeleton import read_skeleton

ADDON = __package__

# Model files are named by the ELF hash of the object they dress, and a skin number.
CHARACTERS = {
    0x007183BF: "Kameo", 0x0CE65812: "Pummel Weed", 0x0A70BAB2: "Snare", 0x037AABBF: "Major Ruin", 0x079B8925: "Rubble",
    0x0BBEAEDE: "Ash", 0x005C444C: "Thermite", 0x05800519: "Chilla", 0x008B5284: "40 Below", 0x08EBDF9E: "Flex", 0x056D64DE: "Deep Blue",
}
KAMEO_OUTFITS = {0: "default outfit", 1: "second outfit", 2: "third outfit"}


def _preferences():
    return bpy.context.preferences.addons[ADDON].preferences


def _assets_folder() -> str:
    game = bpy.path.abspath(_preferences().game_folder)
    for candidate in (os.path.join(game, "assets"), game):
        if os.path.isfile(os.path.join(candidate, "default.xex")) or any(f.lower().endswith(".mdl") for f in _listdir(candidate)):
            return candidate
    return ""


def _listdir(folder: str) -> list[str]:
    try:
        return os.listdir(folder)
    except OSError:
        return []


def _mods_folder() -> str:
    assets = _assets_folder()
    return os.path.join(os.path.dirname(assets), "mods") if assets else ""


def model_label(filename: str) -> str:
    match = re.match(r"([0-9A-Fa-f]{8})_(\d+)\.mdl$", filename)
    if not match:
        return filename
    who, number = int(match.group(1), 16), int(match.group(2))
    name = CHARACTERS.get(who, "Unknown character")
    if who == 0x007183BF:
        return f"Kameo, {KAMEO_OUTFITS.get(number, f'outfit {number}')}"
    return f"{name}, skin {number}"


def _model_items(self, context):
    folder = _assets_folder()
    files = sorted(f for f in _listdir(folder) if f.lower().endswith(".mdl"))
    items = [(os.path.join(folder, f), model_label(f), f) for f in files]
    items.sort(key=lambda item: item[1])
    return items or [("", "No models found: set the game folder in the add-on's preferences", "")]


class KameoPreferences(bpy.types.AddonPreferences):
    bl_idname = ADDON

    game_folder: StringProperty(name="Game folder", subtype="DIR_PATH", description="The Kameo RePowered folder: the one holding KameoLauncher.exe and assets")

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "game_folder")
        assets = _assets_folder()
        if assets:
            count = len([f for f in _listdir(assets) if f.lower().endswith(".mdl")])
            layout.label(text=f"Found {count} character models. Mods go to {_mods_folder()}", icon="CHECKMARK")
        else:
            layout.label(text="Choose the folder you installed Kameo RePowered in.", icon="ERROR")


# --- import ----------------------------------------------------------------------------------------------------

def _import_model(context, path: str) -> set[str]:
    from .gltf import export_gltf

    work = tempfile.mkdtemp(prefix="kameo_")
    gltf_path = os.path.join(work, "model.gltf")
    export_gltf(path, gltf_path)
    before_images = set(bpy.data.images)
    before_objects = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=gltf_path)
    for image in set(bpy.data.images) - before_images:
        if image.source == "FILE":
            image.pack()
    character = [o for o in bpy.data.objects if o not in before_objects]
    for o in character:
        o["kameo_character"] = True
    context.scene["kameo_model"] = path
    context.scene["kameo_label"] = model_label(os.path.basename(path))
    return {"FINISHED"}


class KAMEO_OT_import_character(bpy.types.Operator):
    """Open one of the game's characters, textured and rigged, to edit"""
    bl_idname = "kameo.import_character"
    bl_label = "Import Kameo Character"
    bl_options = {"REGISTER", "UNDO"}

    model: EnumProperty(name="Character", items=_model_items)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=400)

    def execute(self, context):
        if not self.model or not os.path.isfile(self.model):
            self.report({"ERROR"}, "Set the game folder in Edit > Preferences > Add-ons > Kameo Modkit first")
            return {"CANCELLED"}
        result = _import_model(context, self.model)
        self.report({"INFO"}, f"Opened {model_label(os.path.basename(self.model))}")
        return result


class KAMEO_OT_import_file(bpy.types.Operator):
    """Open a Kameo model file (.mdl) from anywhere"""
    bl_idname = "kameo.import_file"
    bl_label = "Import Kameo Model File"
    bl_options = {"REGISTER", "UNDO"}

    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.mdl", options={"HIDDEN"})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        return _import_model(context, self.filepath)


# --- attach ----------------------------------------------------------------------------------------------------

def _character_armature(context):
    return next((o for o in context.scene.objects if o.type == "ARMATURE" and o.get("kameo_character")), None) or \
        next((o for o in context.scene.objects if o.type == "ARMATURE"), None)


def _draw_choices(model_path: str):
    """Per draw a copy can be made of: (graph, mesh, draw), the joints its palette follows, how many vertices its
    mesh has, whether its material is the ordinary lit kind (a colour texture, a normal map, nothing else of its own:
    no transparency, reflection or specular map that would tint or cut a new part), and whether it has a normal map."""
    archive = Caff.load(model_path)
    resolver = Resolver(archive)
    out = []
    for graph, entry in enumerate(e for e in archive.entries if resolver.type_name(e) == "rendergraph"):
        palette = texture_palette(resolver, Ref(entry, 0))
        roots = rendergraph_roots(resolver, entry)
        skeleton = read_skeleton(resolver, roots)
        if skeleton is None:
            continue
        for number, mesh in enumerate(collect_meshes(resolver, roots)):
            if mesh.node.type != "ndVertexBuffer2" or (BLENDINDICES, 0) not in mesh.attributes or (TEXCOORD, 0) not in mesh.attributes:
                continue
            for d, draw in enumerate(mesh.draws):
                if draw.material is None or not draw.palette:
                    continue
                samplers = [s for s in read_material(resolver, draw.material).samplers if not s.dynamic and s.palette_index < len(palette) and palette[s.palette_index] is not None]
                if not any(s.channel == "colour" for s in samplers):
                    continue
                normal = any(read_texture(resolver, palette[s.palette_index]).format_name == "DXN" for s in samplers)
                ordinary = len(samplers) == 1 + normal
                joints = {skeleton.joint_of_matrix(m) for m in draw.palette}
                out.append(((graph, number, d), joints, len(mesh.positions), ordinary, normal, len(draw.palette)))
    return out


def _nearest_bone(armature, point: Vector, usable: set[str]) -> str | None:
    best, best_distance = None, math.inf
    for bone in armature.data.bones:
        if usable and bone.name not in usable:
            continue
        head = armature.matrix_world @ bone.head_local
        tail = armature.matrix_world @ bone.tail_local
        # Distance to the bone's segment.
        axis = tail - head
        t = 0.0 if axis.length_squared == 0 else max(0.0, min(1.0, (point - head).dot(axis) / axis.length_squared))
        distance = (head + axis * t - point).length
        if distance < best_distance:
            best, best_distance = bone.name, distance
    return best


def _bone_items(self, context):
    armature = _character_armature(context)
    items = [("AUTO", "Nearest bone", "The bone closest to the part")]
    if armature:
        items += [(b.name, b.name, "") for b in armature.data.bones]
    return items


def _solid_image(name: str, colour) -> bpy.types.Image:
    image = bpy.data.images.new(name, 32, 32, alpha=True)
    image.pixels = [colour[0], colour[1], colour[2], colour[3] if len(colour) > 3 else 1.0] * (32 * 32)
    image.pack()
    return image


def _base_colour_image(material) -> bool:
    """Gives a material a base colour image if it has none, from its base colour: a game draw needs a texture. An
    image whose file is missing (an FBX naming a path on the modeller's own drive, a painted image never saved) is
    replaced the same way."""
    if not material.use_nodes:
        material.use_nodes = True
    tree = material.node_tree
    shader = next((n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if shader is None:
        return False
    socket = shader.inputs["Base Color"]
    if socket.is_linked:
        source = socket.links[0].from_node
        if source.type == "TEX_IMAGE" and (source.image is None or source.image.size[0] == 0):
            source.image = _solid_image(material.name + "_colour", socket.default_value)
        return True
    texture = tree.nodes.new("ShaderNodeTexImage")
    texture.image = _solid_image(material.name + "_colour", socket.default_value)
    tree.links.new(texture.outputs["Color"], socket)
    return True


def _normal_image(material) -> None:
    """A flat normal map for a material without one, when the draw it copies has one: else it would show the
    template's bumps."""
    tree = material.node_tree
    shader = next((n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if shader is None:
        return
    if shader.inputs["Normal"].is_linked:
        # A normal map with an image that loads is kept. An FBX import links a Normal Map node with nothing behind it,
        # and a file can be missing: those export no normal map, and the part would show the template's.
        node = shader.inputs["Normal"].links[0].from_node
        while node is not None and node.type != "TEX_IMAGE":
            linked = next((i for i in node.inputs if i.is_linked), None)
            node = linked.links[0].from_node if linked else None
        if node is not None and node.image is not None and node.image.size[0] > 0:
            return
        tree.links.remove(shader.inputs["Normal"].links[0])
    image = _solid_image(material.name + "_normal", (0.5, 0.5, 1.0, 1.0))
    image.colorspace_settings.name = "Non-Color"
    texture = tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    normal_map = tree.nodes.new("ShaderNodeNormalMap")
    tree.links.new(texture.outputs["Color"], normal_map.inputs["Color"])
    tree.links.new(normal_map.outputs["Normal"], shader.inputs["Normal"])


def _character_meshes(context):
    return [o for o in context.scene.objects if o.type == "MESH" and o.get("kameo_character") and o.vertex_groups]


def _weight_source(context):
    """One mesh holding the character's skin weights, to copy them from, and a function that removes it. It is made
    from the character's own draws in the scene (not parts attached earlier, which are copies of draws and would pass
    on only their own weights), or, where none are left, from the game's file again."""
    imported = []
    source = _joined_copy(context, _character_meshes(context), originals_only=True)
    if source is None:
        from .gltf import export_gltf

        work = tempfile.mkdtemp(prefix="kameo_weights_")
        export_gltf(context.scene["kameo_model"], os.path.join(work, "model.gltf"))
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(work, "model.gltf"))
        imported = [o for o in bpy.data.objects if o not in before]
        source = _joined_copy(context, [o for o in imported if o.type == "MESH" and o.vertex_groups], originals_only=False)

    def remove():
        for o in [source] + imported:
            if o.name in bpy.data.objects:
                bpy.data.objects.remove(o, do_unlink=True)

    return source, remove


def _joined_copy(context, meshes, originals_only: bool):
    """The meshes copied and joined into one, transforms applied; with `originals_only`, only the faces of the game's
    own draws (graph<g>_mesh<m>_draw<d>, no copy suffix). None when no face is left."""
    if not meshes:
        return None
    copies = []
    for o in meshes:
        copy = o.copy()
        copy.data = o.data.copy()
        copy.modifiers.clear()
        copy.parent = None
        copy.matrix_world = o.matrix_world
        context.scene.collection.objects.link(copy)
        copies.append(copy)
    bpy.ops.object.select_all(action="DESELECT")
    for copy in copies:
        copy.select_set(True)
    context.view_layer.objects.active = copies[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if len(copies) > 1:
        bpy.ops.object.join()
    source = context.view_layer.objects.active
    if originals_only:
        own = [bool(slot.material and re.fullmatch(r"graph\d+_mesh\d+_draw\d+", slot.material.name)) for slot in source.material_slots]
        mesh = bmesh.new()
        mesh.from_mesh(source.data)
        attached = [f for f in mesh.faces if not (f.material_index < len(own) and own[f.material_index])]
        bmesh.ops.delete(mesh, geom=attached, context="FACES")
        mesh.to_mesh(source.data)
        mesh.free()
        if not source.data.polygons:
            bpy.data.objects.remove(source, do_unlink=True)
            return None
    return source


def _copy_weights(context, part) -> None:
    """The character's skin weights onto the part, from the nearest point of its surface; four bones a vertex at
    most, as the game skins."""
    source, remove = _weight_source(context)
    try:
        for group in list(part.vertex_groups):
            part.vertex_groups.remove(group)
        bpy.ops.object.select_all(action="DESELECT")
        part.select_set(True)
        context.view_layer.objects.active = part
        modifier = part.modifiers.new("kameo_weights", "DATA_TRANSFER")
        modifier.object = source
        modifier.use_vert_data = True
        modifier.data_types_verts = {"VGROUP_WEIGHTS"}
        modifier.vert_mapping = "POLYINTERP_NEAREST"
        modifier.layers_vgroup_select_src = "ALL"
        modifier.layers_vgroup_select_dst = "NAME"
        bpy.ops.object.datalayout_transfer(modifier=modifier.name)
        bpy.ops.object.modifier_apply(modifier=modifier.name)
        bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=4)
        bpy.ops.object.vertex_group_normalize_all(lock_active=False)
    finally:
        remove()


def _target_object(context, armature, name: str):
    """The character's mesh object of that name, made again (empty, on the armature) if it was deleted."""
    if name in bpy.data.objects and bpy.data.objects[name].name in context.scene.objects:
        return bpy.data.objects[name]
    obj = bpy.data.objects.new(name, bpy.data.meshes.new(name))
    context.scene.collection.objects.link(obj)
    obj.parent = armature
    obj.matrix_parent_inverse = armature.matrix_world.inverted()
    obj.modifiers.new("Armature", "ARMATURE").object = armature
    obj["kameo_character"] = True
    return obj


class KAMEO_OT_attach(bpy.types.Operator):
    """Make the selected model part of the character: it follows the bone it sits nearest (or the one chosen), keeps
    its own textures, and is exported with the character"""
    bl_idname = "kameo.attach"
    bl_label = "Attach to Character"
    bl_options = {"REGISTER", "UNDO"}

    mode: EnumProperty(name="Moves", items=[
        ("AUTO", "Automatic", "Big pieces bend with the body, small ones follow the nearest bone"),
        ("BODY", "Bend with the body", "Clothes, hair, a whole new body: copies the character's skin weights from the nearest part of her"),
        ("BONE", "Follow one bone", "Hats, props, armour plates: moves rigidly with one bone"),
        ("KEEP", "Keep its weights", "Already weighted to the character's bones (joint_..): keeps them"),
    ], default="AUTO")
    bone: EnumProperty(name="Bone", items=_bone_items, description="The bone the part follows, when it follows one")

    @classmethod
    def poll(cls, context):
        return context.scene.get("kameo_model") and any(o.type == "MESH" and not o.get("kameo_character") for o in context.selected_objects)

    def execute(self, context):
        armature = _character_armature(context)
        if armature is None:
            self.report({"ERROR"}, "Import a Kameo character first (File > Import > Kameo Character)")
            return {"CANCELLED"}
        parts = [o for o in context.selected_objects if o.type == "MESH" and not o.get("kameo_character")]
        bpy.ops.object.mode_set(mode="OBJECT") if context.mode != "OBJECT" else None

        # The part as one object, its transforms applied.
        bpy.ops.object.select_all(action="DESELECT")
        for o in parts:
            o.select_set(True)
            o.parent = None
        context.view_layer.objects.active = parts[0]
        if len(parts) > 1:
            bpy.ops.object.join()
        part = context.view_layer.objects.active
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        centre = sum((part.matrix_world @ Vector(c) for c in part.bound_box), Vector()) / 8

        choices = _draw_choices(context.scene["kameo_model"])
        if not choices:
            self.report({"ERROR"}, "This character has no part a new one can be modelled on")
            return {"CANCELLED"}
        mode = self.mode
        if mode == "AUTO" and any(g.name.startswith("joint_") for g in part.vertex_groups):
            mode = "KEEP"
        if mode == "AUTO":
            height = max((armature.matrix_world @ b.head_local).z for b in armature.data.bones) - min((armature.matrix_world @ b.head_local).z for b in armature.data.bones)
            corners = [part.matrix_world @ Vector(c) for c in part.bound_box]
            mode = "BODY" if max(c.z for c in corners) - min(c.z for c in corners) > 0.25 * height else "BONE"
        bone = self.bone if self.bone != "AUTO" else _nearest_bone(armature, centre, set())
        joint = int(bone.split("_")[1])

        # The draw to copy: the ordinary lit material (the body's, which a new part looks right in), with room. A part
        # bending with the body wants the biggest bone palette (fewer splits); one on a bone, a palette following it.
        vertices = len(part.data.vertices)
        def score(choice):
            (g, m, d), joints, count, ordinary, normal, capacity = choice
            name = f"graph{g}_mesh{m}"
            current = len(bpy.data.objects[name].data.vertices) if name in bpy.data.objects and bpy.data.objects[name].type == "MESH" else 0
            if current + vertices * 2 > 65000:
                return -1
            follows = capacity / 12 if mode == "BODY" else (joint in joints)
            return ordinary * 8 + normal * 4 + follows * 2 + count / 100000
        best = max(choices, key=score)
        if score(best) < 0:
            self.report({"ERROR"}, "No part of this character has room for this model; use one with fewer vertices")
            return {"CANCELLED"}
        (g, m, d), _, _, _, template_normal, _ = best
        target = _target_object(context, armature, f"graph{g}_mesh{m}")
        template = f"graph{g}_mesh{m}_draw{d}"

        # Materials become copies of the template's, each with an image.
        taken = {mat.name for mat in bpy.data.materials}
        number = 1
        for slot in part.material_slots:
            if slot.material is None:
                slot.material = bpy.data.materials.new("part")
            if slot.material.users > 1:
                slot.material = slot.material.copy()
            while f"{template}.{number:03d}" in taken:
                number += 1
            slot.material.name = f"{template}.{number:03d}"
            taken.add(slot.material.name)
            _base_colour_image(slot.material)
            if template_normal:
                _normal_image(slot.material)
        if not part.material_slots:
            material = bpy.data.materials.new(f"{template}.{number:03d}")
            material.use_nodes = True
            part.data.materials.append(material)
            _base_colour_image(material)
            if template_normal:
                _normal_image(material)

        # UVs and colours under the mesh's names, the whole part on the bone.
        if not part.data.uv_layers:
            part.data.uv_layers.new()
        part.data.uv_layers[0].name = target.data.uv_layers[0].name if target.data.uv_layers else "UVMap"
        if target.data.color_attributes and target.data.color_attributes[0].name not in part.data.color_attributes:
            source = target.data.color_attributes[0]
            values = [c.color[:] for c in source.data] or [(1.0, 1.0, 1.0, 1.0)]
            average = [sum(v[k] for v in values) / len(values) for k in range(4)]
            colour = part.data.color_attributes.new(source.name, source.data_type, source.domain)
            for c in colour.data:
                c.color = average
        if mode == "BODY":
            _copy_weights(context, part)
        elif mode == "KEEP":
            for group in [g for g in part.vertex_groups if not g.name.startswith("joint_")]:
                part.vertex_groups.remove(group)
        else:
            for group in list(part.vertex_groups):
                part.vertex_groups.remove(group)
            part.vertex_groups.new(name=bone).add(range(len(part.data.vertices)), 1.0, "REPLACE")
        for modifier in list(part.modifiers):
            part.modifiers.remove(modifier)

        bpy.ops.object.select_all(action="DESELECT")
        part.select_set(True)
        target.select_set(True)
        context.view_layer.objects.active = target
        bpy.ops.object.join()
        self.report({"INFO"}, {"BODY": "Attached: it bends with the body", "KEEP": "Attached with its own weights"}.get(mode, f"Attached to {bone}") + ", and is exported with the character")
        return {"FINISHED"}


# --- export ----------------------------------------------------------------------------------------------------

def _png_images(gltf_path: str) -> None:
    """Images the glTF export wrote as JPEG, saved again as PNG: the importer reads PNG."""
    with open(gltf_path, encoding="utf-8") as handle:
        doc = json.load(handle)
    changed = False
    folder = os.path.dirname(gltf_path)
    settings = bpy.context.scene.render.image_settings
    saved = (settings.file_format, settings.color_mode, settings.color_depth)
    try:
        settings.file_format, settings.color_mode, settings.color_depth = "PNG", "RGBA", "8"
        for image in doc.get("images", []):
            uri = image.get("uri", "")
            if uri.lower().endswith(".png") or "bufferView" in image:
                continue
            source = bpy.data.images.get(image.get("name", "")) or next((i for i in bpy.data.images if os.path.splitext(os.path.basename(i.filepath))[0] == os.path.splitext(os.path.basename(uri))[0]), None)
            if source is None:
                raise RuntimeError(f"the image {uri} is not a PNG and was not found to convert")
            png = os.path.splitext(uri)[0] + ".png"
            source.save_render(os.path.join(folder, png))
            image["uri"], image["mimeType"] = png, "image/png"
            changed = True
    finally:
        settings.file_format, settings.color_mode, settings.color_depth = saved
    if changed:
        with open(gltf_path, "w", encoding="utf-8") as handle:
            json.dump(doc, handle)


def _unique_image_names(objects) -> None:
    """The images the character shows renamed apart: the glTF export names an image's file by its name up to the last
    dot, so two models' "tex005.png" and "tex005.png.001" would be written to one file and one would show the other."""
    images = []
    for o in objects:
        for slot in getattr(o, "material_slots", []):
            tree = slot.material.node_tree if slot.material and slot.material.use_nodes else None
            for node in tree.nodes if tree else []:
                if node.type == "TEX_IMAGE" and node.image and node.image not in images:
                    images.append(node.image)
    taken: set[str] = set()
    for image in images:
        stem = re.sub(r"[^A-Za-z0-9_-]+", "_", image.name).strip("_") or "image"
        name, number = stem, 1
        while name.lower() in taken:
            number += 1
            name = f"{stem}_{number}"
        taken.add(name.lower())
        if image.name != name:
            image.name = name


def _enable_in_launcher(mods: str, filename: str) -> None:
    order = os.path.join(mods, "load_order.txt")
    lines = []
    if os.path.isfile(order):
        with open(order, encoding="utf-8") as handle:
            lines = [line.rstrip("\n") for line in handle if line.strip() and line.strip()[1:] != filename]
    lines.append("+" + filename)
    with open(order, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


class KAMEO_OT_export_mod(bpy.types.Operator):
    """Save the character as a mod for Kameo RePowered, ready to play"""
    bl_idname = "kameo.export_mod"
    bl_label = "Export Kameo Mod"
    bl_options = {"REGISTER"}

    mod_name: StringProperty(name="Mod name", default="My Kameo Mod")
    author: StringProperty(name="Author", default="")
    description: StringProperty(name="Description", default="")
    enable: BoolProperty(name="Turn it on in the launcher", default=True)

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model"))

    def invoke(self, context, event):
        if self.mod_name == "My Kameo Mod" and context.scene.get("kameo_label"):
            self.mod_name = f"My {context.scene['kameo_label']}"
        return context.window_manager.invoke_props_dialog(self, width=420)

    def execute(self, context):
        original = context.scene["kameo_model"]
        mods = _mods_folder()
        if not os.path.isfile(original):
            self.report({"ERROR"}, f"The character's game file is missing: {original}")
            return {"CANCELLED"}
        if not mods:
            self.report({"ERROR"}, "Set the game folder in Edit > Preferences > Add-ons > Kameo Modkit first")
            return {"CANCELLED"}
        try:
            filename, notes = make_mod(context, original, mods, self.mod_name, self.author, self.description, self.enable)
        except ModError as error:
            self.report({"ERROR"}, f"The mod could not be made: {error}")
            return {"CANCELLED"}
        for note in notes:
            self.report({"INFO"}, note)
        self.report({"INFO"}, f"Saved {filename} to the launcher's mods folder" + (" and turned it on" if self.enable else ""))
        return {"FINISHED"}


class ModError(Exception):
    pass


def make_mod(context, original: str, mods: str, name: str, author: str, description: str, enable: bool) -> tuple[str, list[str]]:
    """The scene's character as a .kmod in `mods` (switched on in the launcher's load order when `enable`): its file
    name and the importer's notes. Raises ModError, in words a modder can act on."""
    from . import kmod
    from .importer import ImportError_, import_gltf

    armature = _character_armature(context)
    work = tempfile.mkdtemp(prefix="kameo_mod_")
    gltf_path = os.path.join(work, "model.gltf")

    # Only the character: its armature and what hangs from it.
    bpy.ops.object.mode_set(mode="OBJECT") if context.mode != "OBJECT" else None
    bpy.ops.object.select_all(action="DESELECT")
    for o in context.scene.objects:
        if o == armature or (armature and o.parent == armature) or o.get("kameo_character"):
            o.select_set(True)
    _unique_image_names(context.selected_objects)
    bpy.ops.export_scene.gltf(filepath=gltf_path, export_format="GLTF_SEPARATE", use_selection=True)
    try:
        _png_images(gltf_path)
        out = os.path.join(work, os.path.basename(original))
        notes = import_gltf(original, gltf_path, out)
    except (ImportError_, RuntimeError, ValueError) as error:
        raise ModError(str(error)) from error
    with open(original, "rb") as a, open(out, "rb") as b:
        patch = kmod.diff(os.path.basename(original), a.read(), b.read())
    os.makedirs(mods, exist_ok=True)
    filename = re.sub(r"[^A-Za-z0-9_ -]+", "", name).strip().replace(" ", "_") + ".kmod"
    with open(os.path.join(mods, filename), "wb") as handle:
        handle.write(kmod.write([patch], name, author, "1.0", description))
    if enable:
        _enable_in_launcher(mods, filename)
    return filename, notes


# --- panel and menus -------------------------------------------------------------------------------------------

class KAMEO_PT_panel(bpy.types.Panel):
    bl_label = "Kameo"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Kameo"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        if not scene.get("kameo_model"):
            layout.operator(KAMEO_OT_import_character.bl_idname, text="Open a Character", icon="ARMATURE_DATA")
            layout.label(text="Set the game folder in the add-on's preferences first.")
            return
        layout.label(text=scene.get("kameo_label", ""), icon="ARMATURE_DATA")
        box = layout.box()
        box.label(text="Add something to the character:")
        box.label(text="1. Import or model it, place it on her.")
        box.label(text="   (A whole new body: delete hers first.)")
        box.label(text="2. Select it, then:")
        box.operator(KAMEO_OT_attach.bl_idname, icon="CONSTRAINT_BONE")
        box = layout.box()
        box.label(text="Edit freely: reshape, paint textures,")
        box.label(text="delete parts. Then:")
        box.operator(KAMEO_OT_export_mod.bl_idname, icon="EXPORT")


def _menu_import(self, context):
    self.layout.operator(KAMEO_OT_import_character.bl_idname, text="Kameo Character")
    self.layout.operator(KAMEO_OT_import_file.bl_idname, text="Kameo Model File (.mdl)")


def _menu_export(self, context):
    self.layout.operator(KAMEO_OT_export_mod.bl_idname, text="Kameo Mod (.kmod)")


CLASSES = (KameoPreferences, KAMEO_OT_import_character, KAMEO_OT_import_file, KAMEO_OT_attach, KAMEO_OT_export_mod, KAMEO_PT_panel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
