# Kameo Modkit for Blender 2.79: the same Import > Kameo Character and Export > Kameo Mod as the add-on for newer
# Blender, for those modelling in 2.79. Blender 2.79's Python is too old to run the modkit, so the conversions run in
# a newer Blender (2.93 or later; the free portable zip from blender.org is enough) in the background, and models
# cross between the two as FBX. Written for Python 3.5: no f-strings, 2.79's property syntax.

bl_info = {
    "name": "Kameo Modkit (Blender 2.79)",
    "author": "Kameo RePowered",
    "version": (1, 0, 0),
    "blender": (2, 79, 0),
    "location": "File > Import > Kameo Character, File > Export > Kameo Mod, Tool Shelf > Kameo",
    "description": "Edit Kameo: Elements of Power characters and save them as mods for Kameo RePowered's launcher",
    "category": "Import-Export",
}

import os
import re
import subprocess
import tempfile

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "kameo_modkit", "bridge.py")

# Model files are named by the hash of the character they dress and a skin number.
CHARACTERS = {
    0x007183BF: "Kameo", 0x0CE65812: "Pummel Weed", 0x0A70BAB2: "Snare", 0x037AABBF: "Major Ruin", 0x079B8925: "Rubble",
    0x0BBEAEDE: "Ash", 0x005C444C: "Thermite", 0x05800519: "Chilla", 0x008B5284: "40 Below", 0x08EBDF9E: "Flex", 0x056D64DE: "Deep Blue",
}
KAMEO_OUTFITS = {0: "default outfit", 1: "second outfit", 2: "third outfit"}


def model_label(filename):
    match = re.match(r"([0-9A-Fa-f]{8})_(\d+)\.mdl$", filename)
    if not match:
        return filename
    who, number = int(match.group(1), 16), int(match.group(2))
    if who == 0x007183BF:
        return "Kameo, " + KAMEO_OUTFITS.get(number, "outfit {}".format(number))
    return "{}, skin {}".format(CHARACTERS.get(who, "Unknown character"), number)


def preferences():
    return bpy.context.user_preferences.addons[__name__].preferences


def assets_folder():
    game = bpy.path.abspath(preferences().game_folder)
    for candidate in (os.path.join(game, "assets"), game):
        if os.path.isdir(candidate) and any(f.lower().endswith(".mdl") for f in os.listdir(candidate)):
            return candidate
    return ""


def mods_folder():
    assets = assets_folder()
    return os.path.join(os.path.dirname(assets), "mods") if assets else ""


# 2.79 drops dynamic enum strings Python no longer references, so the list is kept.
_model_cache = []


def model_items(self, context):
    del _model_cache[:]
    folder = assets_folder()
    files = sorted(f for f in (os.listdir(folder) if folder else []) if f.lower().endswith(".mdl"))
    items = sorted(((os.path.join(folder, f), model_label(f), f) for f in files), key=lambda item: item[1])
    _model_cache.extend(items or [("", "No models found: set the game folder in the add-on's preferences", "")])
    return _model_cache


def run_bridge(arguments):
    """Runs the helper in the newer Blender: (finished, notes, error)."""
    blender = bpy.path.abspath(preferences().blender_path)
    if not os.path.isfile(blender):
        return False, [], "set 'Newer Blender' in the add-on's preferences to a blender.exe of version 2.93 or later"
    flags = 0x08000000 if os.name == "nt" else 0  # no console window
    process = subprocess.run([blender, "-b", "--factory-startup", "--python", BRIDGE, "--"] + arguments,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, creationflags=flags)
    lines = process.stdout.splitlines()
    notes = [line[6:] for line in lines if line.startswith("NOTE: ")]
    errors = [line[7:] for line in lines if line.startswith("ERROR: ")]
    done = any(line.startswith("DONE: ") for line in lines)
    if errors or not done:
        return False, notes, errors[-1] if errors else "the helper Blender stopped: " + " | ".join(lines[-5:])
    return True, notes, ""


class KameoPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    game_folder = StringProperty(name="Game folder", subtype="DIR_PATH", description="The Kameo RePowered folder: the one holding KameoLauncher.exe and assets")
    blender_path = StringProperty(name="Newer Blender", subtype="FILE_PATH", description="blender.exe of Blender 2.93 or later (the portable zip from blender.org is enough); it does the conversions in the background")

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "game_folder")
        layout.prop(self, "blender_path")
        if assets_folder():
            layout.label(text="Mods go to " + mods_folder(), icon="FILE_TICK")
        else:
            layout.label(text="Choose the folder you installed Kameo RePowered in.", icon="ERROR")
        if not os.path.isfile(bpy.path.abspath(self.blender_path)):
            layout.label(text="Choose a blender.exe of version 2.93 or later: it converts the models.", icon="ERROR")


class KameoImportCharacter(bpy.types.Operator):
    """Open one of the game's characters, textured and rigged, to edit"""
    bl_idname = "kameo.import_character"
    bl_label = "Import Kameo Character"
    bl_options = {"REGISTER", "UNDO"}

    model = EnumProperty(name="Character", items=model_items)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=400)

    def execute(self, context):
        if not self.model or not os.path.isfile(self.model):
            self.report({"ERROR"}, "Set the game folder in File > User Preferences > Add-ons > Kameo Modkit first")
            return {"CANCELLED"}
        fbx = os.path.join(tempfile.mkdtemp(prefix="kameo_"), "character.fbx")
        finished, notes, error = run_bridge(["import", self.model, fbx])
        if not finished:
            self.report({"ERROR"}, "Could not open it: " + error)
            return {"CANCELLED"}
        bpy.ops.import_scene.fbx(filepath=fbx)
        bpy.ops.file.pack_all()
        context.scene["kameo_model"] = self.model
        context.scene["kameo_label"] = model_label(os.path.basename(self.model))
        self.report({"INFO"}, "Opened " + context.scene["kameo_label"])
        return {"FINISHED"}


class KameoExportMod(bpy.types.Operator):
    """Save the character as a mod for Kameo RePowered, ready to play. Anything in the scene that is not Kameo's own
    becomes part of her: weighted to her joint_ bones, it keeps those weights; if not, it takes hers from the nearest
    part of her body (big pieces) or follows the nearest bone (small ones)"""
    bl_idname = "kameo.export_mod"
    bl_label = "Export Kameo Mod"
    bl_options = {"REGISTER"}

    mod_name = StringProperty(name="Mod name", default="My Kameo Mod")
    author = StringProperty(name="Author", default="")
    description = StringProperty(name="Description", default="")
    enable = BoolProperty(name="Turn it on in the launcher", default=True)

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model"))

    def invoke(self, context, event):
        if self.mod_name == "My Kameo Mod" and context.scene.get("kameo_label"):
            self.mod_name = "My " + context.scene["kameo_label"]
        return context.window_manager.invoke_props_dialog(self, width=420)

    def execute(self, context):
        model = context.scene["kameo_model"]
        if not os.path.isfile(model):
            self.report({"ERROR"}, "The character's game file is missing: " + model)
            return {"CANCELLED"}
        if not mods_folder():
            self.report({"ERROR"}, "Set the game folder in File > User Preferences > Add-ons > Kameo Modkit first")
            return {"CANCELLED"}
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        fbx = os.path.join(tempfile.mkdtemp(prefix="kameo_mod_"), "scene.fbx")
        bpy.ops.export_scene.fbx(filepath=fbx, use_selection=False, add_leaf_bones=False, bake_anim=False, path_mode="COPY", embed_textures=True)
        finished, notes, error = run_bridge(["mod", fbx, model, mods_folder(), self.mod_name, self.author, self.description, "1" if self.enable else "0"])
        for note in notes:
            self.report({"INFO"}, note)
        if not finished:
            self.report({"ERROR"}, "The mod could not be made: " + error)
            return {"CANCELLED"}
        self.report({"INFO"}, "Saved '{}' to the launcher's mods folder{}".format(self.mod_name, " and turned it on" if self.enable else ""))
        return {"FINISHED"}


class KameoPanel(bpy.types.Panel):
    bl_label = "Kameo"
    bl_space_type = "VIEW_3D"
    bl_region_type = "TOOLS"
    bl_category = "Kameo"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        if not scene.get("kameo_model"):
            layout.operator(KameoImportCharacter.bl_idname, text="Open a Character", icon="ARMATURE_DATA")
            layout.label(text="Set the game folder and a newer")
            layout.label(text="Blender in the add-on's settings.")
            return
        layout.label(text=scene.get("kameo_label", ""), icon="ARMATURE_DATA")
        box = layout.box()
        box.label(text="Edit freely; add models and rig")
        box.label(text="them to her joint_ bones (or not:")
        box.label(text="the export rigs what isn't). Then:")
        box.operator(KameoExportMod.bl_idname, icon="EXPORT")


def menu_import(self, context):
    self.layout.operator(KameoImportCharacter.bl_idname, text="Kameo Character")


def menu_export(self, context):
    self.layout.operator(KameoExportMod.bl_idname, text="Kameo Mod (.kmod)")


CLASSES = (KameoPreferences, KameoImportCharacter, KameoExportMod, KameoPanel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.INFO_MT_file_import.append(menu_import)
    bpy.types.INFO_MT_file_export.append(menu_export)


def unregister():
    bpy.types.INFO_MT_file_export.remove(menu_export)
    bpy.types.INFO_MT_file_import.remove(menu_import)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
