"""Her animations in Blender: the game's own clips played on the character's armature, to see how a model's weights
hold up in a run, a jump or a fight without starting the game.

A clip (animation.py) gives each joint a rotation, a translation added to the joint's own and a scale. The game builds
a joint's matrix as its parent's times translate x rotate x scale (BuildMatricesFromJoints), and skins with that matrix
times a move by minus the joint's bind position (skeletonSubtractPivots). The same matrices are worked out here, taken
from the game's space to Blender's (X negated for the glTF, then Y-up to Z-up, as the import did to the model) and
written as an action on the armature: every pose bone keyed on every frame, at 60 frames a second.

The clips have no names in the game's files, only the ids the game asks for them by, so they are listed by number
and length. The character stays in place: a clip's root motion is not read.
"""

from __future__ import annotations

import os

import bpy
import numpy as np
from bpy.props import EnumProperty
from mathutils import Matrix

from . import animation
from .caff import Caff
from .graph import Resolver, rendergraph_roots
from .skeleton import read_skeleton

# Game space to the armature's: x -> -x (glTF), then glTF's (x, y, z) -> Blender's (x, -z, y).
_TO_BLENDER = np.array([[-1.0, 0.0, 0.0, 0.0], [0.0, 0.0, -1.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0]])
# Where a character type's clips are: by its joint count, the archive holding them. Kameo's are in the shared archive.
CLIP_ARCHIVES = {81: "011C2CC3.lvl"}

_cache: dict = {}


def _rotation(q: np.ndarray) -> np.ndarray:
    """Rotation matrices (n, 3, 3) of unit quaternions (n, 4) x y z w, for column vectors."""
    x, y, z, w = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    m = np.empty((len(q), 3, 3))
    m[:, 0, 0] = 1 - 2 * (y * y + z * z); m[:, 0, 1] = 2 * (x * y - z * w); m[:, 0, 2] = 2 * (x * z + y * w)
    m[:, 1, 0] = 2 * (x * y + z * w); m[:, 1, 1] = 1 - 2 * (x * x + z * z); m[:, 1, 2] = 2 * (y * z - x * w)
    m[:, 2, 0] = 2 * (x * z - y * w); m[:, 2, 1] = 2 * (y * z + x * w); m[:, 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def skin_matrices(skeleton, motion: animation.Motion) -> np.ndarray:
    """Per frame and joint, the matrix that moves a bind-pose vertex of that joint, in the game's space."""
    frames, joints = motion.rotation.shape[:2]
    joints = min(joints, len(skeleton.joints))
    world = np.zeros((frames, joints, 4, 4))
    out = np.zeros((frames, joints, 4, 4))
    for j in range(joints):
        joint = skeleton.joints[j]
        local = np.zeros((frames, 4, 4))
        local[:, :3, :3] = _rotation(motion.rotation[:, j].astype(np.float64)) * motion.scale[:, j, None, :]
        local[:, :3, 3] = np.array(joint.translation) + motion.translation[:, j]
        local[:, 3, 3] = 1.0
        world[:, j] = world[:, joint.parent] @ local if 0 <= joint.parent < j else local
        unpivot = np.eye(4)
        unpivot[:3, 3] = [-c for c in joint.pivot]
        out[:, j] = world[:, j] @ unpivot
    return out


def build_action(armature, skeleton, motion: animation.Motion, name: str):
    """An action on the armature playing the clip: every joint's bone keyed on every frame."""
    matrices = skin_matrices(skeleton, motion)
    frames, joints = matrices.shape[:2]
    back = np.linalg.inv(_TO_BLENDER)
    matrices = _TO_BLENDER @ matrices @ back
    bones = {}
    for bone in armature.data.bones:
        parts = bone.name.split("_")
        if len(parts) == 2 and parts[0] == "joint" and parts[1].isdigit() and int(parts[1]) < joints:
            bones[int(parts[1])] = bone
    rest = {j: np.array(b.matrix_local) for j, b in bones.items()}
    action = bpy.data.actions.new(name)
    times = np.arange(frames, dtype=np.float64) + 1.0
    pose = {j: matrices[:, j] @ rest[j] for j in bones}
    for j, bone in bones.items():
        parent = next((int(p.name.split("_")[1]) for p in bone.parent_recursive if p.name.startswith("joint_") and int(p.name.split("_")[1]) in bones), None) if bone.parent else None
        if parent is None:
            basis = np.linalg.inv(rest[j]) @ pose[j]
        else:
            rest_local = np.linalg.inv(rest[parent]) @ rest[j]
            basis = np.linalg.inv(rest_local) @ (np.linalg.inv(pose[parent]) @ pose[j])
        location = np.empty((frames, 3)); rotation = np.empty((frames, 4)); scale = np.empty((frames, 3))
        last = None
        for f in range(frames):
            loc, quat, size = Matrix(basis[f].tolist()).decompose()
            if last is not None and quat.dot(last) < 0.0:
                quat.negate()
            last = quat.copy()
            location[f] = loc; rotation[f] = quat; scale[f] = size
        path = f'pose.bones["{bone.name}"].'
        for prop, values in (("location", location), ("rotation_quaternion", rotation), ("scale", scale)):
            for index in range(values.shape[1]):
                curve = action.fcurves.new(path + prop, index=index, action_group=bone.name)
                curve.keyframe_points.add(frames)
                curve.keyframe_points.foreach_set("co", np.stack([times, values[:, index]], axis=1).reshape(-1))
                curve.update()
    for bone in armature.pose.bones:
        bone.rotation_mode = "QUATERNION"
    return action


def _clips(context):
    """The clips for the scene's character and the archive they are in: (path, resolver, skeleton, clips), or None."""
    from .blender import _assets_folder, _character_armature

    model = context.scene.get("kameo_model", "")
    armature = _character_armature(context)
    if not model or armature is None or not os.path.isfile(model):
        return None
    key = model
    if key not in _cache:
        archive = Caff.load(model)
        resolver = Resolver(archive)
        skeleton = next((s for s in (read_skeleton(resolver, rendergraph_roots(resolver, e)) for e in archive.entries if resolver.type_name(e) == "rendergraph") if s), None)
        found = None
        if skeleton is not None:
            name = CLIP_ARCHIVES.get(len(skeleton.joints))
            try:
                assets = _assets_folder()
            except KeyError:  # the add-on's preferences are not there when it is run without being installed
                assets = ""
            folders = [assets, os.path.dirname(model), os.path.join(os.path.dirname(os.path.dirname(model)), "assets")]
            path = next((os.path.join(f, name) for f in folders if name and f and os.path.isfile(os.path.join(f, name))), None)
            if path:
                clips_resolver = Resolver(Caff.load(path))
                found = (path, clips_resolver, skeleton, animation.list_clips(clips_resolver, len(skeleton.joints)))
        _cache[key] = found
    return _cache[key]


_items: list = []


def _clip_items(self, context):
    found = _clips(context)
    items = [(str(c.entry), f"{i + 1:03d}   {c.seconds:.1f} s   ({c.ident:08X})", f"{c.frames} frames") for i, c in enumerate(found[3])] if found else []
    _items[:] = items or [("0", "No animations found for this character", "")]
    return _items


class KAMEO_OT_play_clip(bpy.types.Operator):
    """Play one of the character's own animations on the armature, to see how the model bends"""
    bl_idname = "kameo.play_clip"
    bl_label = "Play Her Animation"
    bl_options = {"REGISTER", "UNDO"}
    bl_property = "clip"

    clip: EnumProperty(name="Animation", items=_clip_items)

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model"))

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        from .blender import _character_armature

        found = _clips(context)
        if not found or self.clip == "0":
            self.report({"ERROR"}, "No animations were found for this character: only Kameo's are known so far, and they need the game folder set in the add-on's preferences")
            return {"CANCELLED"}
        _, resolver, skeleton, clips = found
        clip = next(c for c in clips if str(c.entry) == self.clip)
        try:
            motion = animation.read_motion(resolver, resolver.archive.entries[clip.entry - 1])
        except (ValueError, IndexError) as error:
            self.report({"ERROR"}, f"That animation could not be read: {error}")
            return {"CANCELLED"}
        armature = _character_armature(context)
        number = clips.index(clip) + 1
        action = build_action(armature, skeleton, motion, f"kameo_{number:03d}")
        if armature.animation_data is None:
            armature.animation_data_create()
        old = armature.animation_data.action
        armature.animation_data.action = action
        if old is not None and old.name.startswith("kameo_") and old.users == 0:
            bpy.data.actions.remove(old)
        scene = context.scene
        scene.render.fps, scene.render.fps_base = animation.FRAMES_PER_SECOND, 1.0
        scene.frame_start, scene.frame_end = 1, max(clip.frames, 1)
        scene.frame_set(1)
        if not context.screen.is_animation_playing:
            bpy.ops.screen.animation_play()
        self.report({"INFO"}, f"Playing animation {number} ({clip.seconds:.1f} s). Space stops it; Stand Still puts her back in her T-pose")
        return {"FINISHED"}


class KAMEO_OT_rest_pose(bpy.types.Operator):
    """Stop the animation and stand the character in her T-pose again"""
    bl_idname = "kameo.rest_pose"
    bl_label = "Stand Still"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        from .blender import _character_armature

        armature = _character_armature(context)
        if armature is None:
            return {"CANCELLED"}
        if context.screen and context.screen.is_animation_playing:
            bpy.ops.screen.animation_cancel(restore_frame=False)
        if armature.animation_data and armature.animation_data.action:
            action = armature.animation_data.action
            armature.animation_data.action = None
            if action.name.startswith("kameo_") and action.users == 0:
                bpy.data.actions.remove(action)
        for bone in armature.pose.bones:
            bone.matrix_basis = Matrix.Identity(4)
        return {"FINISHED"}


def draw(layout, scene):
    row = layout.row(align=True)
    row.operator(KAMEO_OT_play_clip.bl_idname, icon="PLAY")
    row.operator(KAMEO_OT_rest_pose.bl_idname, text="", icon="ARMATURE_DATA")


CLASSES = (KAMEO_OT_play_clip, KAMEO_OT_rest_pose)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    _cache.clear()
