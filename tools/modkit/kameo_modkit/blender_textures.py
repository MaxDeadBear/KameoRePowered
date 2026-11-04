"""The add-on's Textures panel: the character's textures, painted in Blender, shown in the running game.

Every image the import brings in knows the texture it came from (kameo_textures.json, written beside the PNGs): the
hashes the game names it by, its size and format. Show in Game writes each image that differs from the game's into
<game>/textures/replace/blender/<model>/ as <hash>_<width>x<height>_fmt<format>.png, the name a texture dump gives
it, and removes the files of images that no longer differ; the game watches that folder and swaps them in while it
runs (kameo_gfx_texture_live_reload), and F8 there switches replacements off and on to compare. Live Link does the
same a moment after each brush stroke.

Replacement images are not part of a .kmod: they stay in the game's textures folder, and any image size works.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import zlib

import bpy
import numpy as np
from bpy.props import BoolProperty, StringProperty

from .texture import write_png

MANIFEST = "kameo_textures.json"
LIVE_SECONDS = 1.0
KIND_LABEL = {"colour": "Colour", "normal": "Normal map", "mask": "Mask"}

# Per image, the checksum of the pixels last written to the game (None: its files were removed), so Live Link writes
# only what changed since.
_sent: dict[str, int | None] = {}


def _game_folder() -> str:
    from .blender import _assets_folder

    assets = _assets_folder()
    return os.path.dirname(assets) if assets else ""


def _replace_folder(context) -> str:
    game = _game_folder()
    model = context.scene.get("kameo_model", "")
    return os.path.join(game, "textures", "replace", "blender", os.path.splitext(os.path.basename(model))[0]) if game and model else ""


def _pixels(image) -> np.ndarray:
    """The image as (height, width, 4) bytes, top row first."""
    width, height = image.size
    values = np.empty(width * height * 4, np.float32)
    image.pixels.foreach_get(values)
    return np.flipud((np.clip(values, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).reshape(height, width, 4))


def _checksum(pixels: np.ndarray) -> int:
    return zlib.crc32(pixels.tobytes(), zlib.crc32(np.array(pixels.shape, np.int32).tobytes()))


def game_images() -> list:
    return sorted((i for i in bpy.data.images if i.get("kameo_game")), key=lambda i: i.name)


def tag_imported_images(images, textures_folder: str) -> int:
    """Marks the images a glTF import made with the game texture each came from; how many it knew."""
    try:
        with open(os.path.join(textures_folder, MANIFEST), encoding="utf-8") as handle:
            manifest = json.load(handle)
    except OSError:
        return 0
    known = 0
    for image in images:
        entry = manifest.get(os.path.basename(bpy.path.abspath(image.filepath_raw)))
        if entry is None or image.size[0] == 0:
            continue
        image["kameo_game"] = json.dumps(entry)
        image["kameo_original"] = str(_checksum(_pixels(image)))
        known += 1
    return known


def rekey_images(context, hashes: dict[int, list[str]], before: str) -> None:
    """After a costume is saved: each image names the costume's texture (its own hashes, the painting now baked in),
    and the images shown live before it, under the old names, are taken out of the game."""
    for folder in {before, _replace_folder(context)}:
        if folder:
            shutil.rmtree(folder, ignore_errors=True)
    _sent.clear()
    for image in game_images():
        entry = _entry(image)
        if entry.get("entry") in hashes:
            entry["hashes"] = hashes[entry["entry"]]
            image["kameo_game"] = json.dumps(entry)
            image["kameo_original"] = str(_checksum(_pixels(image)))


def _entry(image) -> dict:
    return json.loads(image["kameo_game"])


def _edited(image, pixels: np.ndarray) -> bool:
    return str(_checksum(pixels)) != image.get("kameo_original")


def _for_game(pixels: np.ndarray, kind: str) -> np.ndarray:
    # A mask was opened as white with its brightness as alpha; the game's texture is the brightness.
    if kind == "mask":
        grey = pixels[:, :, 3:4]
        return np.concatenate([grey, grey, grey, np.full_like(grey, 255)], axis=2)
    return pixels


def _file_names(entry: dict) -> list[str]:
    return [f"{h}_{entry['width']}x{entry['height']}_fmt{entry['format']}.png" for h in entry["hashes"]]


def sync(context, only_changed: bool = False) -> tuple[int, int]:
    """Writes every edited image to the game's replace folder and removes the files of the rest: (shown, removed).
    With `only_changed`, images whose pixels are what was last written are left alone (Live Link)."""
    folder = _replace_folder(context)
    if not folder:
        raise RuntimeError("set the game folder in the add-on's preferences first")
    os.makedirs(folder, exist_ok=True)
    shown = removed = 0
    for image in game_images():
        if only_changed and not image.is_dirty and image.name in _sent:
            continue
        entry = _entry(image)
        pixels = _pixels(image)
        edited = _edited(image, pixels)
        checksum = _checksum(pixels) if edited else None
        if only_changed and _sent.get(image.name, "unsent") == checksum:
            continue
        for name in _file_names(entry):
            path = os.path.join(folder, name)
            if edited:
                # Written aside and renamed, so the game never reads half a file.
                write_png(path + ".tmp", _for_game(pixels, entry["kind"]))
                os.replace(path + ".tmp", path)
            elif os.path.exists(path):
                os.remove(path)
                removed += 1
        shown += edited
        _sent[image.name] = checksum
    return shown, removed


def _game_running() -> bool:
    if os.name != "nt":
        return False
    try:
        listing = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, creationflags=0x08000000).stdout.lower()
    except OSError:
        return False
    return '"kameorepowered.exe"' in listing or '"kameorepowered-tu.exe"' in listing


# --- operators -------------------------------------------------------------------------------------------------

class KAMEO_OT_show_textures(bpy.types.Operator):
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
            self.report({"ERROR"}, f"Could not write the textures: {error}")
            return {"CANCELLED"}
        if shown:
            self.report({"INFO"}, f"{shown} texture(s) now show in game" + ("" if _game_running() else "; start the game to see them"))
        else:
            self.report({"INFO"}, "No texture differs from the game's yet: paint one first" + (f" ({removed} put back)" if removed else ""))
        return {"FINISHED"}


class KAMEO_OT_remove_textures(bpy.types.Operator):
    """Take this character's painted textures out of the game, so it shows its own again"""
    bl_idname = "kameo.remove_textures"
    bl_label = "Remove from Game"

    @classmethod
    def poll(cls, context):
        folder = _replace_folder(context)
        return bool(folder) and os.path.isdir(folder)

    def execute(self, context):
        shutil.rmtree(_replace_folder(context), ignore_errors=True)
        _sent.clear()
        context.scene.kameo_live_textures = False
        self.report({"INFO"}, "The game shows this character's own textures again")
        return {"FINISHED"}


class KAMEO_OT_paint_texture(bpy.types.Operator):
    """Paint this texture on the model"""
    bl_idname = "kameo.paint_texture"
    bl_label = "Paint"
    bl_options = {"REGISTER", "UNDO"}

    image: StringProperty()

    def execute(self, context):
        image = bpy.data.images.get(self.image)
        if image is None:
            return {"CANCELLED"}
        # A mesh of the character with a material showing the image, that image's node made the paint target.
        found = None
        for o in context.scene.objects:
            if o.type != "MESH" or not o.get("kameo_character"):
                continue
            for index, slot in enumerate(o.material_slots):
                tree = slot.material.node_tree if slot.material and slot.material.use_nodes else None
                node = next((n for n in tree.nodes if n.type == "TEX_IMAGE" and n.image == image), None) if tree else None
                if node:
                    found = (o, index, slot.material, node)
                    break
            if found:
                break
        if found is None:
            self.report({"WARNING"}, f"No part of the character shows {image.name}")
            return {"CANCELLED"}
        obj, index, material, node = found
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        obj.hide_set(False)
        obj.select_set(True)
        context.view_layer.objects.active = obj
        obj.active_material_index = index
        material.node_tree.nodes.active = node
        bpy.ops.object.mode_set(mode="TEXTURE_PAINT")
        context.scene.tool_settings.image_paint.mode = "MATERIAL"
        for area in context.screen.areas:
            if area.type == "IMAGE_EDITOR":
                area.spaces.active.image = image
        self.report({"INFO"}, f"Painting {image.name}: {'Live Link shows each stroke in game' if context.scene.kameo_live_textures else 'press Show in Game to see it there'}")
        return {"FINISHED"}


class KAMEO_OT_play(bpy.types.Operator):
    """Start Kameo RePowered as the launcher's Play button does, with your mods switched on"""
    bl_idname = "kameo.play"
    bl_label = "Play"

    def execute(self, context):
        game = _game_folder()
        launcher = os.path.join(game, "KameoLauncher.exe")
        if not os.path.isfile(launcher):
            self.report({"ERROR"}, "KameoLauncher.exe is not in the game folder; set it in the add-on's preferences")
            return {"CANCELLED"}
        if _game_running():
            self.report({"INFO"}, "The game is already running: painted textures update in it as you go")
            return {"FINISHED"}
        if game_images():
            sync(context)
        subprocess.Popen([launcher, "--play"], cwd=game, creationflags=0x08000000 if os.name == "nt" else 0)
        self.report({"INFO"}, "Kameo is starting")
        return {"FINISHED"}


# --- live link -------------------------------------------------------------------------------------------------

def _live_tick():
    context = bpy.context
    scene = getattr(context, "scene", None)
    if scene is None or not scene.kameo_live_textures:
        return None
    try:
        sync(context, only_changed=True)
    except (OSError, RuntimeError):
        pass
    return LIVE_SECONDS


def _live_changed(scene, context):
    if scene.kameo_live_textures:
        _sent.clear()
        if not bpy.app.timers.is_registered(_live_tick):
            bpy.app.timers.register(_live_tick, first_interval=0.1)


# --- panel -----------------------------------------------------------------------------------------------------

class KAMEO_PT_textures(bpy.types.Panel):
    bl_label = "Textures"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Kameo"
    bl_parent_id = "KAMEO_PT_panel"

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model"))

    def draw(self, context):
        layout = self.layout
        images = game_images()
        if not images:
            layout.label(text="Open the character again to paint its textures.", icon="INFO")
            return
        if not context.scene.get("kameo_costume"):
            box = layout.box()
            box.label(text="Painting here changes this costume", icon="ERROR")
            box.label(text="in game. Save as New Costume first")
            box.label(text="to paint a copy of your own.")
        column = layout.column(align=True)
        for image in images:
            entry = _entry(image)
            row = column.row(align=True)
            preview = image.preview_ensure() if hasattr(image, "preview_ensure") else image.preview
            label = f"{KIND_LABEL.get(entry['kind'], entry['kind'])} {entry['width']}×{entry['height']}"
            in_game = _sent.get(image.name) is not None
            row.label(text=label, icon_value=preview.icon_id if preview else 0)
            if in_game:
                row.label(text="", icon="CHECKMARK")
            row.operator(KAMEO_OT_paint_texture.bl_idname, text="", icon="BRUSH_DATA").image = image.name
        layout.separator()
        row = layout.row(align=True)
        row.scale_y = 1.3
        row.operator(KAMEO_OT_show_textures.bl_idname, icon="EXPORT")
        row.prop(context.scene, "kameo_live_textures", text="Live Link", toggle=True, icon="LINKED")
        layout.operator(KAMEO_OT_remove_textures.bl_idname, icon="TRASH")
        layout.label(text="In game, F8 switches them off and on.", icon="INFO")


class KAMEO_PT_play(bpy.types.Panel):
    bl_label = "Try It"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Kameo"
    bl_parent_id = "KAMEO_PT_panel"

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model"))

    def draw(self, context):
        row = self.layout.row()
        row.scale_y = 1.4
        row.operator(KAMEO_OT_play.bl_idname, icon="PLAY")


CLASSES = (KAMEO_OT_show_textures, KAMEO_OT_remove_textures, KAMEO_OT_paint_texture, KAMEO_OT_play, KAMEO_PT_textures, KAMEO_PT_play)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.kameo_live_textures = BoolProperty(name="Live Link", description="Send each brush stroke to the running game a moment after it", update=_live_changed)


def unregister():
    if bpy.app.timers.is_registered(_live_tick):
        bpy.app.timers.unregister(_live_tick)
    del bpy.types.Scene.kameo_live_textures
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
