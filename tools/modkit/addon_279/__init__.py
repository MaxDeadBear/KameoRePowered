# Kameo Modkit for Blender 2.79: the same Import > Kameo Character, Export > Kameo Mod, Textures panel and Play as the
# add-on for newer Blender, for those modelling in 2.79. Blender 2.79's Python is too old to run the modkit, so the
# conversions run in a newer Blender (2.93 or later; the free portable zip from blender.org is enough) in the
# background, and models cross between the two as FBX. Painted textures need no conversion: they are written straight
# into the game's textures/replace folder, which the running game watches (see kameo_modkit/blender_textures.py).
# Written for Python 3.5: no f-strings, 2.79's property syntax, and no NumPy.

bl_info = {
    "name": "Kameo Modkit (Blender 2.79)",
    "author": "Kameo RePowered",
    "version": (1, 1, 0),
    "blender": (2, 79, 0),
    "location": "File > Import > Kameo Character, File > Export > Kameo Mod, Tool Shelf > Kameo",
    "description": "Edit Kameo: Elements of Power characters, paint them live in the game, and save them as mods for Kameo RePowered's launcher",
    "category": "Import-Export",
}

import array
import importlib.machinery
import json
import os
import shutil
import struct
import subprocess
import tempfile
import zlib

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "kameo_modkit", "bridge.py")
NO_WINDOW = 0x08000000 if os.name == "nt" else 0
LIVE_SECONDS = 1.0
KIND_LABEL = {"colour": "Colour", "normal": "Normal map", "mask": "Mask"}

# The character names both add-ons use; the rest of the modkit needs a newer Python than 2.79's.
names = importlib.machinery.SourceFileLoader("kameo_modkit_names", os.path.join(HERE, "kameo_modkit", "names.py")).load_module()
model_label = names.model_label


def preferences():
    return bpy.context.user_preferences.addons[__name__].preferences


def assets_folder():
    game = bpy.path.abspath(preferences().game_folder)
    for candidate in (os.path.join(game, "assets"), game):
        if os.path.isdir(candidate) and any(f.lower().endswith(".mdl") for f in os.listdir(candidate)):
            return candidate
    return ""


def game_folder():
    assets = assets_folder()
    return os.path.dirname(assets) if assets else ""


def mods_folder():
    game = game_folder()
    return os.path.join(game, "mods") if game else ""


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
    process = subprocess.run([blender, "-b", "--factory-startup", "--python", BRIDGE, "--"] + arguments,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, creationflags=NO_WINDOW)
    lines = process.stdout.splitlines()
    notes = [line[6:] for line in lines if line.startswith("NOTE: ")]
    errors = [line[7:] for line in lines if line.startswith("ERROR: ")]
    done = any(line.startswith("DONE: ") for line in lines)
    if errors or not done:
        return False, notes, errors[-1] if errors else "the helper Blender stopped: " + " | ".join(lines[-5:])
    return True, notes, ""


def game_running():
    if os.name != "nt":
        return False
    try:
        listing = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], stdout=subprocess.PIPE, universal_newlines=True, creationflags=NO_WINDOW).stdout.lower()
    except OSError:
        return False
    return '"kameorepowered.exe"' in listing or '"kameorepowered-tu.exe"' in listing


# --- textures --------------------------------------------------------------------------------------------------
# Each image the import brings in carries the game texture it came from (kameo_game: hashes, size, format, kind) and a
# checksum of its pixels as opened; one whose pixels differ is written to <game>/textures/replace/blender/<model>/
# under the name a texture dump gives it, and the game swaps it in.

# Per image, the checksum last written to the game (None: its files were removed), for Live Link.
_sent = {}


def replace_folder(context):
    game = game_folder()
    model = context.scene.get("kameo_model", "")
    if not game or not model:
        return ""
    return os.path.join(game, "textures", "replace", "blender", os.path.splitext(os.path.basename(model))[0])


def image_bytes(image):
    """The image as RGBA bytes, top row first, and its width and height."""
    width, height = image.size
    values = image.pixels[:]
    try:
        import numpy
    except ImportError:
        numpy = None
    if numpy is not None:
        pixels = (numpy.clip(numpy.array(values, numpy.float32), 0.0, 1.0) * 255.0 + 0.5).astype(numpy.uint8)
        return numpy.flipud(pixels.reshape(height, width * 4)).tobytes(), width, height
    row = width * 4
    rows = []
    for y in range(height - 1, -1, -1):
        rows.append(bytes(min(255, max(0, int(v * 255.0 + 0.5))) for v in values[y * row:(y + 1) * row]))
    return b"".join(rows), width, height


def checksum(image):
    width, height = image.size
    return zlib.crc32(array.array("f", image.pixels[:]).tobytes(), zlib.crc32(struct.pack("<II", width, height)))


def write_png(path, rgba, width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def for_game(rgba, kind):
    # A mask was opened as white with its brightness as alpha; the game's texture is the brightness.
    if kind != "mask":
        return rgba
    out = bytearray(len(rgba))
    out[0::4] = out[1::4] = out[2::4] = rgba[3::4]
    out[3::4] = b"\xff" * (len(rgba) // 4)
    return bytes(out)


def game_images():
    return sorted((i for i in bpy.data.images if i.get("kameo_game")), key=lambda i: i.name)


def tag_images(images, mapping):
    """Marks the images an FBX import made with the game texture each came from. The FBX import makes an image per
    material using a texture, so the copies are merged into one: painting it paints every part that shows it, and no
    unpainted copy takes the painted one's file out of the game."""
    first = {}
    for image in images:
        keys = [image.name, os.path.basename(bpy.path.abspath(image.filepath_raw))]
        keys += [os.path.splitext(k)[0] + ".png" for k in keys]
        entry = next((mapping[k] for k in keys if k in mapping), None)
        if entry is None or image.size[0] == 0:
            continue
        kept = first.get(entry["hashes"][0])
        if kept is not None:
            image.user_remap(kept)
            bpy.data.images.remove(image, do_unlink=True)
            continue
        first[entry["hashes"][0]] = image
        image["kameo_game"] = json.dumps(entry)
        image["kameo_original"] = str(checksum(image))
    return len(first)


def sync(context, only_changed=False):
    """Writes every edited image to the game's replace folder and removes the files of the rest: (shown, removed)."""
    folder = replace_folder(context)
    if not folder:
        raise RuntimeError("set the game folder in the add-on's preferences first")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    shown = removed = 0
    for image in game_images():
        if only_changed and not image.is_dirty and image.name in _sent:
            continue
        entry = json.loads(image["kameo_game"])
        now = checksum(image)
        edited = str(now) != image.get("kameo_original")
        if only_changed and _sent.get(image.name, "unsent") == (now if edited else None):
            continue
        rgba = None
        for h in entry["hashes"]:
            path = os.path.join(folder, "{}_{}x{}_fmt{}.png".format(h, entry["width"], entry["height"], entry["format"]))
            if edited:
                if rgba is None:
                    pixels, width, height = image_bytes(image)
                    rgba = for_game(pixels, entry["kind"])
                # Written aside and renamed, so the game never reads half a file.
                write_png(path + ".tmp", rgba, width, height)
                os.replace(path + ".tmp", path)
            elif os.path.exists(path):
                os.remove(path)
                removed += 1
        shown += edited
        _sent[image.name] = now if edited else None
    return shown, removed


# --- operators -------------------------------------------------------------------------------------------------

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
        before = set(bpy.data.images)
        bpy.ops.import_scene.fbx(filepath=fbx)
        mapping = {}
        if os.path.isfile(fbx + ".textures.json"):
            with open(fbx + ".textures.json", encoding="utf-8") as handle:
                mapping = json.load(handle)
        tag_images([i for i in bpy.data.images if i not in before], mapping)
        bpy.ops.file.pack_all()
        for o in context.scene.objects:
            o["kameo_character"] = True
        context.scene["kameo_model"] = self.model
        context.scene["kameo_label"] = model_label(os.path.basename(self.model))
        _sent.clear()
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


class KameoShowTextures(bpy.types.Operator):
    """Put the textures you changed into the game. It swaps them in while it runs; F8 there switches them off and on"""
    bl_idname = "kameo.show_textures"
    bl_label = "Show in Game"

    @classmethod
    def poll(cls, context):
        return bool(game_images())

    def execute(self, context):
        try:
            shown, removed = sync(context)
        except (OSError, RuntimeError) as error:
            self.report({"ERROR"}, "Could not write the textures: {}".format(error))
            return {"CANCELLED"}
        if shown:
            self.report({"INFO"}, "{} texture(s) now show in game{}".format(shown, "" if game_running() else "; start the game to see them"))
        else:
            self.report({"INFO"}, "No texture differs from the game's yet: paint one first")
        return {"FINISHED"}


class KameoRemoveTextures(bpy.types.Operator):
    """Take this character's painted textures out of the game, so it shows its own again"""
    bl_idname = "kameo.remove_textures"
    bl_label = "Remove from Game"

    @classmethod
    def poll(cls, context):
        folder = replace_folder(context)
        return bool(folder) and os.path.isdir(folder)

    def execute(self, context):
        shutil.rmtree(replace_folder(context), ignore_errors=True)
        _sent.clear()
        context.scene.kameo_live_textures = False
        self.report({"INFO"}, "The game shows this character's own textures again")
        return {"FINISHED"}


class KameoLiveLink(bpy.types.Operator):
    """Send each brush stroke to the running game a moment after it. Press again to stop"""
    bl_idname = "kameo.live_link"
    bl_label = "Live Link"

    _timer = None

    @classmethod
    def poll(cls, context):
        return bool(game_images())

    def invoke(self, context, event):
        scene = context.scene
        if scene.kameo_live_textures:
            scene.kameo_live_textures = False
            return {"FINISHED"}
        scene.kameo_live_textures = True
        _sent.clear()
        self._timer = context.window_manager.event_timer_add(LIVE_SECONDS, context.window)
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def modal(self, context, event):
        if not context.scene.kameo_live_textures:
            context.window_manager.event_timer_remove(self._timer)
            return {"CANCELLED"}
        if event.type == "TIMER":
            try:
                sync(context, only_changed=True)
            except (OSError, RuntimeError):
                pass
        return {"PASS_THROUGH"}


class KameoPaintTexture(bpy.types.Operator):
    """Paint this texture on the model"""
    bl_idname = "kameo.paint_texture"
    bl_label = "Paint"
    bl_options = {"REGISTER", "UNDO"}

    image = StringProperty()

    def execute(self, context):
        image = bpy.data.images.get(self.image)
        if image is None:
            return {"CANCELLED"}
        obj = None
        for o in context.scene.objects:
            if o.type != "MESH":
                continue
            for slot in o.material_slots:
                material = slot.material
                textures = [t.texture for t in (material.texture_slots if material else []) if t and t.texture and t.texture.type == "IMAGE"]
                tree = material.node_tree if material and material.use_nodes else None
                nodes = [n for n in (tree.nodes if tree else []) if n.type == "TEX_IMAGE"]
                if any(t.image == image for t in textures) or any(n.image == image for n in nodes):
                    obj = o
                    break
            if obj:
                break
        if obj is None:
            self.report({"WARNING"}, "No part of the character shows " + image.name)
            return {"CANCELLED"}
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        obj.hide = False
        obj.select = True
        context.scene.objects.active = obj
        bpy.ops.object.mode_set(mode="TEXTURE_PAINT")
        paint = context.scene.tool_settings.image_paint
        paint.mode = "IMAGE"
        paint.canvas = image
        for area in context.screen.areas:
            if area.type == "IMAGE_EDITOR":
                area.spaces.active.image = image
        self.report({"INFO"}, "Painting " + image.name + (": Live Link shows each stroke in game" if context.scene.kameo_live_textures else ": press Show in Game to see it there"))
        return {"FINISHED"}


class KameoPlay(bpy.types.Operator):
    """Start Kameo RePowered as the launcher's Play button does, with your mods switched on"""
    bl_idname = "kameo.play"
    bl_label = "Play"

    def execute(self, context):
        game = game_folder()
        launcher = os.path.join(game, "KameoLauncher.exe")
        if not os.path.isfile(launcher):
            self.report({"ERROR"}, "KameoLauncher.exe is not in the game folder; set it in the add-on's preferences")
            return {"CANCELLED"}
        if game_running():
            self.report({"INFO"}, "The game is already running: painted textures update in it as you go")
            return {"FINISHED"}
        if game_images():
            sync(context)
        subprocess.Popen([launcher, "--play"], cwd=game, creationflags=NO_WINDOW)
        self.report({"INFO"}, "Kameo is starting")
        return {"FINISHED"}


# --- panels ----------------------------------------------------------------------------------------------------

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
        row = layout.row()
        row.scale_y = 1.4
        row.operator(KameoPlay.bl_idname, icon="PLAY")


class KameoTexturesPanel(bpy.types.Panel):
    bl_label = "Kameo Textures"
    bl_space_type = "VIEW_3D"
    bl_region_type = "TOOLS"
    bl_category = "Kameo"

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model"))

    def draw(self, context):
        layout = self.layout
        images = game_images()
        if not images:
            layout.label(text="Open the character again to paint", icon="INFO")
            layout.label(text="its textures.")
            return
        column = layout.column(align=True)
        for image in images:
            entry = json.loads(image["kameo_game"])
            row = column.row(align=True)
            label = "{} {}x{}".format(KIND_LABEL.get(entry["kind"], entry["kind"]), entry["width"], entry["height"])
            row.label(text=label, icon_value=image.preview.icon_id if getattr(image, "preview", None) else 0)
            if _sent.get(image.name) is not None:
                row.label(text="", icon="FILE_TICK")
            row.operator(KameoPaintTexture.bl_idname, text="", icon="BRUSH_DATA").image = image.name
        layout.separator()
        column = layout.column(align=True)
        column.scale_y = 1.3
        column.operator(KameoShowTextures.bl_idname, icon="EXPORT")
        live = context.scene.kameo_live_textures
        column.operator(KameoLiveLink.bl_idname, text="Live Link: on" if live else "Live Link: off", icon="LINKED" if live else "UNLINKED")
        layout.operator(KameoRemoveTextures.bl_idname, icon="X")
        layout.label(text="In game, F8 switches them", icon="INFO")
        layout.label(text="off and on.")


def menu_import(self, context):
    self.layout.operator(KameoImportCharacter.bl_idname, text="Kameo Character")


def menu_export(self, context):
    self.layout.operator(KameoExportMod.bl_idname, text="Kameo Mod (.kmod)")


CLASSES = (KameoPreferences, KameoImportCharacter, KameoExportMod, KameoShowTextures, KameoRemoveTextures, KameoLiveLink, KameoPaintTexture, KameoPlay, KameoPanel, KameoTexturesPanel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.kameo_live_textures = BoolProperty(name="Live Link", default=False)
    bpy.types.INFO_MT_file_import.append(menu_import)
    bpy.types.INFO_MT_file_export.append(menu_export)


def unregister():
    bpy.types.INFO_MT_file_export.remove(menu_export)
    bpy.types.INFO_MT_file_import.remove(menu_import)
    del bpy.types.Scene.kameo_live_textures
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
