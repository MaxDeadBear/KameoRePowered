"""Live Link for the model: edits to a costume's mesh show on the character in the running game.

The game reloads a costume's file on whoever wears it when the file is written again (src/engine/dlc/mod_costumes.cpp),
so the link is Save Costume pressed for you: a moment after the last change to a mesh (its vertices, its weights, its
materials' UVs and colours), the costume is saved as the button saves it. It works on a costume, since a costume is the
one kind of model the game takes while it runs; a character opened from the game's own files is saved as a costume
first.

A save builds the whole model, which takes a second or two and leaves Edit Mode to read the mesh, so the link waits
until editing has paused and puts the mode, the active object and the selection back afterwards.
"""

from __future__ import annotations

import time

import bpy
from bpy.app.handlers import persistent
from bpy.props import BoolProperty

# How long edits must have stopped before the costume is sent.
DELAY = 1.2

_changed_at = 0.0
_saving = False

# bpy.context.mode's names, as object.mode_set takes them.
_MODES = {
    "EDIT_MESH": "EDIT",
    "PAINT_WEIGHT": "WEIGHT_PAINT",
    "PAINT_VERTEX": "VERTEX_PAINT",
    "PAINT_TEXTURE": "TEXTURE_PAINT",
    "SCULPT": "SCULPT",
    "POSE": "POSE",
    "OBJECT": "OBJECT",
}


def _live(scene) -> bool:
    return bool(scene is not None and scene.get("kameo_costume") and getattr(scene, "kameo_model_live", False))


@persistent
def _on_change(scene, depsgraph):
    """Notes a change to a mesh. Posing the armature moves the mesh too, but a pose is not part of the model."""
    global _changed_at
    if _saving or not _live(scene) or bpy.context.mode == "POSE":
        return
    for update in depsgraph.updates:
        if update.is_updated_geometry and isinstance(update.id, (bpy.types.Mesh, bpy.types.Object)):
            if isinstance(update.id, bpy.types.Object) and update.id.type != "MESH":
                continue
            _changed_at = time.monotonic()
            return


def _restore(scene, mode, active, selected):
    view_layer = bpy.context.view_layer
    if bpy.context.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    for obj in scene.objects:
        obj.select_set(obj.name in selected)
    if active is not None and active.name in scene.objects:
        view_layer.objects.active = active
        wanted = _MODES.get(mode, "OBJECT")
        if wanted != "OBJECT":
            bpy.ops.object.mode_set(mode=wanted)


def _send(scene):
    """Saves the costume as the button does, and leaves the modder where they were."""
    global _changed_at, _saving
    _changed_at = 0.0
    _saving = True
    mode = bpy.context.mode
    active = bpy.context.view_layer.objects.active
    selected = {obj.name for obj in scene.objects if obj.select_get()}
    try:
        # What is being edited is only in the mesh once Edit Mode writes it there.
        for obj in scene.objects:
            if obj.type == "MESH" and obj.mode == "EDIT":
                obj.update_from_editmode()
        result = bpy.ops.kameo.save_costume("EXEC_DEFAULT")
        if "FINISHED" in result:
            scene["kameo_live_status"] = "Sent to the game at " + time.strftime("%H:%M:%S")
        else:
            scene["kameo_live_status"] = "Not sent: the reason is in the status bar"
    except Exception as error:  # an operator that cannot run raises RuntimeError with its report
        scene["kameo_live_status"] = f"Not sent: {error}"
    finally:
        try:
            _restore(scene, mode, active, selected)
        finally:
            _saving = False


def _tick():
    scene = bpy.context.scene
    if not _live(scene):
        return 0.5
    if _changed_at and time.monotonic() - _changed_at >= DELAY:
        window = bpy.context.window or (bpy.context.window_manager.windows[0] if bpy.context.window_manager.windows else None)
        if window is not None and hasattr(bpy.context, "temp_override"):
            with bpy.context.temp_override(window=window):
                _send(scene)
        else:
            _send(scene)
    return 0.3


def _toggled(self, context):
    global _changed_at
    scene = context.scene
    if scene.kameo_model_live:
        scene["kameo_live_status"] = "On: edits are sent when you pause"
        # What is in the scene now goes first, so the game shows this model before the first edit.
        _changed_at = time.monotonic() - DELAY
        if not bpy.app.timers.is_registered(_tick):
            bpy.app.timers.register(_tick, first_interval=0.1)
    else:
        scene["kameo_live_status"] = ""
        _changed_at = 0.0


def draw(layout, scene):
    """The panel's Live Link row, for a costume."""
    layout.prop(scene, "kameo_model_live", text="Live Link to the Game", toggle=True, icon="PLAY")
    status = scene.get("kameo_live_status", "")
    if status:
        layout.label(text=status)
    elif not scene.kameo_model_live:
        layout.label(text="Wear the costume in the game first.", icon="INFO")


def register():
    bpy.types.Scene.kameo_model_live = BoolProperty(
        name="Live Link to the Game",
        description="Sends the costume to the running game a moment after each edit to its mesh or weights; the character wearing it there is reloaded",
        default=False,
        update=_toggled,
        options={"SKIP_SAVE"},
    )
    if _on_change not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_on_change)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.5, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if _on_change in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_change)
    del bpy.types.Scene.kameo_model_live
