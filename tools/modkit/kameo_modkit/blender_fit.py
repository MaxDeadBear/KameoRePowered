"""Fitting other models to the character: weights that hold up when she moves.

Three things, each behind a button or an option of Attach to Character (blender.py):

  tidy_weights     after weights are copied from her body onto a model that has none: what makes a copied skirt tear
                   and a bracelet bend is put right (see the function).
  Attach with Its Rig   a model that comes rigged (Mixamo, Unreal, Unity or VRM humanoids, Rigify, Biped; anything
                   whose bones are named for what they are) keeps its own weights: each of its bones is matched to the
                   joint of hers that does the same job (humanoid.py), the model is stood, posed and proportioned as
                   she is, so its elbows are at her elbows, and its weights are moved onto her joints. Bones she has no
                   joint for (twist bones, hair, a skirt's) give their weights to the bone they hang from.
  Import a Game Model as a Part   any model of the game's, from a level archive too (the townsfolk, the bosses), as
                   a part to attach: its bones are named src_<n>, not joint_<n>, since its joints are not hers.

The fit is worked out here, not by posing the model's armature: every bone gets a transform (a move to her joint, a
turn and a stretch along the bone to her next joint), children starting from their parent's, and the vertices are
skinned with them once. A pose bone cannot stretch along a direction that is not its own axis, and imported rigs' axes
point anywhere.
"""

from __future__ import annotations

import math
import os
import tempfile

import bpy
import numpy as np
from bpy.props import BoolProperty, IntProperty, StringProperty
from mathutils import Matrix, Vector

from . import humanoid

# The chains a fit follows: each role's next joint gives its bone a direction and a length.
_NEXT = {
    "hips": "spine", "spine": "spine1", "spine1": "spine2", "spine2": "chest", "chest": "upper_chest", "upper_chest": "neck", "neck": "head",
    "upper_arm": "forearm", "forearm": "hand", "hand": "middle1", "thigh": "shin", "shin": "foot", "foot": "toe",
    "thumb1": "thumb2", "thumb2": "thumb3", "index1": "index2", "index2": "index3", "middle1": "middle2", "middle2": "middle3",
    "ring1": "ring2", "ring2": "ring3", "little1": "little2", "little2": "little3",
}
_TORSO = ("spine", "spine1", "spine2", "chest", "upper_chest")
# A vertex attribute Attach sets before joining parts: which of the selected objects each vertex came from.
PART_ATTRIBUTE = "kameo_part"


def _joint_number(name: str) -> int | None:
    parts = name.split("_")
    return int(parts[1]) if len(parts) == 2 and parts[0] == "joint" and parts[1].isdigit() else None


def is_kameo(armature) -> bool:
    """Whether the armature is Kameo's own 81 joints, whose roles humanoid.py knows."""
    joints = [b for b in armature.data.bones if _joint_number(b.name) is not None]
    if len(joints) != humanoid.JOINTS:
        return False
    return all((armature.data.bones.get(f"joint_{j:02d}").parent is None) == (p < 0) and
               (p < 0 or armature.data.bones[f"joint_{j:02d}"].parent.name == f"joint_{p:02d}") for j, p in humanoid.PARENT.items())


# --- weights copied from her body, tidied ----------------------------------------------------------------------------

def tidy_weights(part, armature) -> list[str]:
    """Puts right what copying weights from the nearest point of her body gets wrong, and says what it did.

    Loose cloth. A skirt's vertex takes the weights of whichever leg is nearer, so neighbours across the middle take
    different legs, and the first stride pulls the faces between them into spikes. Weights are smoothed along the
    part's own surface wherever neighbours disagree like that: what hugs her keeps her weights, which change little
    from one vertex to the next, and what hangs between two limbs blends between them.
    Skirts. Cloth joined across both legs well below the hips takes the thighs' weights where it had the shins', and
    gives more of them to the hips the lower it hangs: a hem does not follow the knee, and follows a stride part way.
    Stiff pieces. A bracelet, a pouch, a jewel: a small separate piece takes one set of weights for all its vertices
    (its main bone, or its main two when it sits on a joint), so it moves as a piece.
    Her own extras. Weights on the bones only she has (the ponytail, the ears, the chest) go to the body bone they hang
    from: a new body has no ponytail to swing.

    Vertices that share a place (split along UV seams) are treated as one, so seams do not open."""
    mesh = part.data
    count = len(mesh.vertices)
    if count == 0 or not part.vertex_groups:
        return []
    notes = []
    names = [g.name for g in part.vertex_groups]
    weights = np.zeros((count, len(names)), np.float32)
    for vertex in mesh.vertices:
        for g in vertex.groups:
            weights[vertex.index, g.group] = g.weight
    co = np.empty(count * 3, np.float32)
    mesh.vertices.foreach_get("co", co)
    co = co.reshape(count, 3)
    world = np.array(part.matrix_world, np.float32)
    co = co @ world[:3, :3].T + world[:3, 3]
    heads = [armature.matrix_world @ b.head_local for b in armature.data.bones]
    height = max(h.z for h in heads) - min(h.z for h in heads) or 1.0

    # Her own extras, onto the body.
    if is_kameo(armature):
        column = {name: i for i, name in enumerate(names)}
        moved = 0.0
        for i, name in enumerate(list(names)):
            joint = _joint_number(name)
            if joint is None or joint in humanoid.BODY:
                continue
            target = f"joint_{humanoid.body_joint(joint):02d}"
            if target not in column:
                column[target] = len(names)
                names.append(target)
                weights = np.concatenate([weights, np.zeros((count, 1), np.float32)], axis=1)
            moved += float(weights[:, i].sum())
            weights[:, column[target]] += weights[:, i]
            weights[:, i] = 0.0
        if moved > 0.5:
            notes.append("weights on her ponytail, ears and chest bones went to the body bones they hang from")

    # Vertices at one place are one.
    _, cluster = np.unique(np.round(co / (1e-4 * height)).astype(np.int64), axis=0, return_inverse=True)
    cluster = cluster.reshape(-1)
    clusters = int(cluster.max()) + 1
    size = np.bincount(cluster, minlength=clusters).astype(np.float32)
    merged = np.zeros((clusters, weights.shape[1]), np.float32)
    np.add.at(merged, cluster, weights)
    merged /= size[:, None]
    place = np.zeros((clusters, 3), np.float32)
    np.add.at(place, cluster, co)
    place /= size[:, None]
    edges = np.empty(len(mesh.edges) * 2, np.int32)
    mesh.edges.foreach_get("vertices", edges)
    edges = cluster[edges.reshape(-1, 2)]
    edges = np.unique(np.sort(edges[edges[:, 0] != edges[:, 1]], axis=1), axis=0)

    # The separate pieces the part is made of.
    parent = np.arange(clusters)

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for a, b in edges.tolist():
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    root = np.array([find(i) for i in range(clusters)])
    pieces = [np.nonzero(root == piece)[0] for piece in np.unique(root)]

    # Skirts hang from the thighs.
    if is_kameo(armature) and len(edges):
        column = {name: i for i, name in enumerate(names)}

        def columns(roles) -> list[int]:
            return [column[n] for n in (f"joint_{humanoid.ROLES[r]:02d}" for r in roles) if n in column]

        legs = {side: columns([f"{r}.{side}" for r in ("thigh", "shin", "foot", "toe")]) for side in "LR"}
        feet = columns([f"{r}.{side}" for r in ("foot", "toe") for side in "LR"])
        side_weight = {side: merged[:, legs[side]].sum(axis=1) for side in "LR"}
        hip = (armature.matrix_world @ armature.data.bones[f"joint_{humanoid.ROLES['thigh.L']:02d}"].head_local).z
        knee = (armature.matrix_world @ armature.data.bones[f"joint_{humanoid.ROLES['shin.L']:02d}"].head_local).z
        on_left, on_right = side_weight["L"] > 0.5, side_weight["R"] > 0.5
        bridge = (on_left[edges[:, 0]] & on_right[edges[:, 1]]) | (on_right[edges[:, 0]] & on_left[edges[:, 1]])
        skirts = 0
        hips = columns(["hips"])
        # A garment is judged whole: the objects the part was joined from where that is known (a skirt's loose strips
        # are pieces of their own), else each separate piece.
        garments = pieces
        if PART_ATTRIBUTE in mesh.attributes:
            marks = np.empty(count, np.int32)
            mesh.attributes[PART_ATTRIBUTE].data.foreach_get("value", marks)
            of_cluster = np.zeros(clusters, np.int32)
            of_cluster[cluster] = marks
            garments = [np.nonzero(of_cluster == mark)[0] for mark in np.unique(of_cluster)]
        for members in garments:
            inside = np.zeros(clusters, bool)
            inside[members] = True
            total = float(merged[members].sum())
            if total <= 0.0 or float(merged[members][:, feet].sum()) > 0.02 * total:
                continue
            crossing = edges[bridge & inside[edges[:, 0]]]
            if not len(crossing):
                continue
            lowest = float(place[crossing.reshape(-1), 2].min())
            if lowest > hip - 0.35 * (hip - knee):
                continue  # joined only at the crotch: trousers, a body
            for side in "LR":
                thigh = columns([f"thigh.{side}"])
                below = columns([f"shin.{side}"])
                if thigh and below:
                    merged[np.ix_(members, thigh)] += merged[np.ix_(members, below)].sum(axis=1, keepdims=True)
                    merged[np.ix_(members, below)] = 0.0
            # The further down it hangs, the more it stays with the hips: a hem follows a stride only part of the way.
            if hips:
                both = legs["L"] + legs["R"]
                share = 0.6 * np.clip((hip - place[members, 2]) / max(hip - knee, 1e-6), 0.0, 1.0)
                taken = merged[np.ix_(members, both)] * share[:, None]
                merged[np.ix_(members, both)] -= taken
                merged[members, hips[0]] += taken.sum(axis=1)
            skirts += 1
        if skirts:
            notes.append("a skirt (cloth joined across both legs below the hips) follows the thighs and not the shins")

    # Smoothing where neighbours disagree: how much of its weight a vertex does not share with a neighbour, 0 to 1.
    # Her own skin changes by a fraction of that from one vertex to the next; cloth between two limbs jumps by all of it.
    if len(edges):
        degree = np.bincount(edges.reshape(-1), minlength=clusters).astype(np.float32)
        smoothed = np.zeros(clusters, bool)
        for _ in range(80):
            jump = 0.5 * np.abs(merged[edges[:, 0]] - merged[edges[:, 1]]).sum(axis=1)
            rough = np.zeros(clusters, np.float32)
            np.maximum.at(rough, edges[:, 0], jump)
            np.maximum.at(rough, edges[:, 1], jump)
            step = 0.7 * np.clip((rough - 0.3) / 0.3, 0.0, 1.0)
            if float(step.max()) <= 0.0:
                break
            # A ring of neighbours joins in, so a jump is spread and not only moved.
            spread = np.zeros(clusters, np.float32)
            np.maximum.at(spread, edges[:, 0], step[edges[:, 1]])
            np.maximum.at(spread, edges[:, 1], step[edges[:, 0]])
            step = np.maximum(step, 0.6 * spread)
            smoothed |= step > 0.2
            total = np.zeros_like(merged)
            np.add.at(total, edges[:, 0], merged[edges[:, 1]])
            np.add.at(total, edges[:, 1], merged[edges[:, 0]])
            average = total / np.maximum(degree, 1.0)[:, None]
            merged = merged + step[:, None] * (average - merged)
        if float(smoothed.mean()) > 0.005:
            notes.append(f"weights smoothed where neighbouring vertices had taken different bones ({int(smoothed.sum())} vertices: cloth between the legs, sleeves, hair)")

    # Small separate pieces move as one.
    stiff = 0
    for members in pieces:
        if len(members) > 0.25 * clusters:
            continue
        extent = place[members].max(axis=0) - place[members].min(axis=0)
        if float(np.linalg.norm(extent)) > 0.1 * height:
            continue
        mean = merged[members].mean(axis=0)
        order = np.argsort(-mean)
        keep = order[:1] if mean[order[0]] >= 0.65 * mean.sum() else order[:2]
        one = np.zeros_like(mean)
        one[keep] = mean[keep]
        merged[members] = one
        stiff += 1
    if stiff:
        notes.append(f"{stiff} small separate piece{'s' if stiff > 1 else ''} (jewellery, straps) each follow their main bone as one")

    _write_weights(part, names, merged[cluster])
    return notes


def _write_weights(obj, names: list[str], weights: np.ndarray) -> None:
    """The object's vertex groups replaced by `weights` (vertex, name): four a vertex at most, summing to one."""
    for group in list(obj.vertex_groups):
        obj.vertex_groups.remove(group)
    order = np.argsort(-weights, axis=1)[:, :4]
    top = np.take_along_axis(weights, order, axis=1)
    top[top < 1e-4] = 0.0
    total = top.sum(axis=1)
    groups: dict[int, object] = {}
    for vertex in range(weights.shape[0]):
        if total[vertex] <= 0.0:
            continue
        for k in range(order.shape[1]):
            if top[vertex, k] > 0.0:
                column = int(order[vertex, k])
                group = groups.get(column)
                if group is None:
                    group = groups[column] = obj.vertex_groups.new(name=names[column])
                group.add([vertex], float(top[vertex, k] / total[vertex]), "REPLACE")


# --- a rigged model, onto her joints ---------------------------------------------------------------------------------

def _source_rig(context):
    """The armature of the selected model that is not the character's, and the meshes it moves."""
    armatures = []
    for o in context.selected_objects:
        candidates = [o] if o.type == "ARMATURE" else [m.object for m in o.modifiers if m.type == "ARMATURE" and m.object] if o.type == "MESH" else []
        if o.type == "MESH" and o.parent and o.parent.type == "ARMATURE":
            candidates.append(o.parent)
        for armature in candidates:
            if not armature.get("kameo_character") and armature not in armatures:
                armatures.append(armature)
    if not armatures:
        return None, []
    armature = armatures[0]
    meshes = [o for o in context.scene.objects if o.type == "MESH" and not o.get("kameo_character") and
              (o.parent == armature or any(m.type == "ARMATURE" and m.object == armature for m in o.modifiers))]
    return armature, meshes


def match_bones(source, target) -> tuple[dict[str, int], dict[str, str], bool]:
    """Each of the source armature's bones matched to one of the character's joints: bone -> joint, the roles found
    by name (bone -> role), and whether names decided it. Names do when the model's bones say what they are; else a
    bone goes to the joint nearest it, the model standing where the character stands."""
    kameo = is_kameo(target)
    roles: dict[str, str] = {}
    if kameo:
        taken: dict[str, str] = {}
        for bone in source.data.bones:
            role = humanoid.role_of(bone.name)
            # The first bone down the hierarchy to claim a role keeps it.
            if role and role not in taken:
                taken[role] = bone.name
                roles[bone.name] = role
    limbs = sum(1 for r in roles.values() if r.split(".")[0] in ("upper_arm", "forearm", "thigh", "shin"))
    by_name = kameo and "hips" in roles.values() and limbs >= 6
    joints: dict[str, int] = {}
    if by_name:
        # The spine's bones, however many, spread over her five.
        torso = [b.name for b in source.data.bones if roles.get(b.name) in _TORSO]
        torso.sort(key=lambda name: len(source.data.bones[name].parent_recursive))
        hers = [humanoid.ROLES[r] for r in _TORSO]
        for i, name in enumerate(torso):
            slot = round(i * (len(hers) - 1) / (len(torso) - 1)) if len(torso) > 1 else 2
            joints[name] = hers[slot]
            roles[name] = _TORSO[slot]
        for name, role in roles.items():
            if name not in joints:
                joint = humanoid.joint_of_role(role)
                if joint is not None:
                    joints[name] = joint
        for bone in source.data.bones:
            walk = bone
            while walk is not None and walk.name not in joints:
                walk = walk.parent
            joints.setdefault(bone.name, joints[walk.name] if walk is not None else humanoid.ROLES["hips"])
        return joints, roles, True

    candidates = [b for b in target.data.bones if _joint_number(b.name) is not None and (not kameo or _joint_number(b.name) in humanoid.BODY)]
    places = [(target.matrix_world @ b.head_local, _joint_number(b.name)) for b in candidates]
    for bone in source.data.bones:
        head = source.matrix_world @ bone.head_local
        joints[bone.name] = min(places, key=lambda p: (p[0] - head).length_squared)[1]
    return joints, {}, False


def _stretch(origin: Vector, along: Vector, factor: float) -> Matrix:
    """Scales by `factor` along the direction `along`, about `origin`."""
    d = along.normalized()
    m = Matrix.Identity(4)
    for r in range(3):
        for c in range(3):
            m[r][c] += (factor - 1.0) * d[r] * d[c]
    return Matrix.Translation(origin) @ m @ Matrix.Translation(-origin)


def fit_transforms(source, target, joints: dict[str, int], roles: dict[str, str], place: bool, proportions: bool) -> dict[str, Matrix]:
    """Per source bone, the world transform that stands what it moves where the character's matching joint is."""
    def hers(joint: int) -> Vector:
        return target.matrix_world @ target.data.bones[f"joint_{joint:02d}"].head_local

    def head(bone) -> Vector:
        return source.matrix_world @ bone.head_local

    of_role = {role: name for name, role in roles.items()}
    base = Matrix.Identity(4)
    if place and {"hips", "head"} <= set(of_role):
        bones = source.data.bones
        sides = next(((of_role[f"{r}.L"], of_role[f"{r}.R"]) for r in ("upper_arm", "thigh", "hand") if f"{r}.L" in of_role and f"{r}.R" in of_role), None)
        turn = Matrix.Identity(4)
        if sides:
            across = head(bones[sides[0]]) - head(bones[sides[1]])
            wanted = hers(humanoid.ROLES["upper_arm.L"]) - hers(humanoid.ROLES["upper_arm.R"])
            turn = Matrix.Rotation(math.atan2(wanted.y, wanted.x) - math.atan2(across.y, across.x), 4, "Z")
        feet = [of_role[r] for r in ("foot.L", "foot.R") if r in of_role]
        low = min(head(bones[f]).z for f in feet) if feet else min(head(b).z for b in bones)
        her_low = min(hers(humanoid.ROLES["foot.L"]).z, hers(humanoid.ROLES["foot.R"]).z)
        tall = head(bones[of_role["head"]]).z - low
        scale = (hers(humanoid.ROLES["head"]).z - her_low) / tall if tall > 1e-6 else 1.0
        base = Matrix.Scale(scale, 4) @ turn
        hips = base @ head(bones[of_role["hips"]])
        base = Matrix.Translation(hers(humanoid.ROLES["hips"]) - hips) @ base

    transforms: dict[str, Matrix] = {}
    for bone in sorted(source.data.bones, key=lambda b: len(b.parent_recursive)):
        matrix = transforms[bone.parent.name].copy() if bone.parent else base.copy()
        role = roles.get(bone.name)
        if proportions and role and not role.startswith("collar"):
            joint = joints[bone.name]
            matrix = Matrix.Translation(hers(joint) - matrix @ head(bone)) @ matrix
            stem, _, side = role.partition(".")
            following = _NEXT.get(stem)
            while following and (f"{following}.{side}" if side else following) not in of_role:
                following = _NEXT.get(following)
            if following:
                child = source.data.bones[of_role[f"{following}.{side}" if side else following]]
                now = matrix @ head(child) - hers(joint)
                wanted = hers(joints[child.name]) - hers(joint)
                if now.length > 1e-6 and wanted.length > 1e-6:
                    turn = now.rotation_difference(wanted).to_matrix().to_4x4()
                    matrix = Matrix.Translation(hers(joint)) @ turn @ Matrix.Translation(-hers(joint)) @ matrix
                    matrix = _stretch(hers(joint), wanted, wanted.length / now.length) @ matrix
        transforms[bone.name] = matrix
    return transforms


def spring_bones(source, target, meshes, joints: dict[str, int], roles: dict[str, str], transforms: dict[str, Matrix], most: int = 96) -> dict[str, str]:
    """Gives the character a spring bone for each of the model's bones she has no joint for and that moves part of
    it (hair, a skirt, a tail, ears): source bone -> the new bone's name. The new bones stand where the fit puts the
    model's, each hanging from her joint or from the spring bone above it. In game they swing on springs."""
    used = set()
    for obj in meshes:
        names = {g.index: g.name for g in obj.vertex_groups}
        for vertex in obj.data.vertices:
            for g in vertex.groups:
                if g.weight > 0.05:
                    used.add(names[g.group])
    chosen = [b for b in sorted(source.data.bones, key=lambda b: len(b.parent_recursive))
              if b.name not in roles and b.name in used and not humanoid.is_helper(b.name) and b.parent is not None][:most]
    if not chosen:
        return {}
    made: dict[str, str] = {}
    taken = {b.name for b in target.data.bones}
    places = {}
    for bone in chosen:
        stem = "spring_" + "".join(c if c.isalnum() else "_" for c in bone.name.split(":")[-1])[:40]
        if stem[7:].isdigit():
            stem += "_"  # a bare number would read as one of the model's existing joints
        name, number = stem, 1
        while name in taken:
            number += 1
            name = f"{stem}_{number}"
        taken.add(name)
        made[bone.name] = name
        matrix = transforms[bone.name] @ source.matrix_world
        places[bone.name] = (target.matrix_world.inverted() @ (matrix @ bone.head_local), target.matrix_world.inverted() @ (matrix @ bone.tail_local))
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.mode_set(mode="EDIT")
    for bone in chosen:
        new = target.data.edit_bones.new(made[bone.name])
        new.head, new.tail = places[bone.name]
        if (new.tail - new.head).length < 1e-4:
            new.tail = new.head + Vector((0.0, 0.0, -0.1))
        above = bone.parent
        while above is not None and above.name not in made and above.name not in roles:
            above = above.parent
        hangs = made[above.name] if above is not None and above.name in made else f"joint_{joints[bone.name]:02d}"
        new.parent = target.data.edit_bones[hangs]
        new.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    return made


def fit_mesh(obj, source, transforms: dict[str, Matrix], joints: dict[str, int], springs: dict[str, str] | None = None, named=()) -> int:
    """Skins the mesh once with the fit's transforms and moves its weights onto the character's joints. Returns how
    many vertices had no bone of the rig (they follow the rig's root)."""
    mesh = obj.data
    if mesh.shape_keys:
        obj.shape_key_clear()
    count = len(mesh.vertices)
    bones = [b.name for b in source.data.bones]
    column = {name: i for i, name in enumerate(bones)}
    group_bone = {g.index: column[g.name] for g in obj.vertex_groups if g.name in column}
    weights = np.zeros((count, len(bones)), np.float32)
    for vertex in mesh.vertices:
        for g in vertex.groups:
            if g.group in group_bone:
                weights[vertex.index, group_bone[g.group]] = g.weight
    total = weights.sum(axis=1)
    loose = total <= 1e-6
    root = column[next(b.name for b in source.data.bones if b.parent is None)]
    weights[loose, root] = 1.0
    weights /= weights.sum(axis=1)[:, None]

    co = np.empty(count * 3, np.float32)
    mesh.vertices.foreach_get("co", co)
    world = np.array(obj.matrix_world, np.float32)
    co = co.reshape(count, 3) @ world[:3, :3].T + world[:3, 3]
    matrices = np.array([np.array(transforms[name], np.float32) for name in bones])
    posed = np.einsum("vb,bij,vj->vi", weights, matrices[:, :3, :3], co) + weights @ matrices[:, :3, 3]

    for modifier in list(obj.modifiers):
        obj.modifiers.remove(modifier)
    obj.parent = None
    obj.matrix_world = Matrix.Identity(4)
    mesh.vertices.foreach_set("co", posed.astype(np.float32).reshape(-1))
    if mesh.has_custom_normals:
        # Normals kept from the import would point where the surface used to face.
        bpy.context.view_layer.objects.active = obj
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    mesh.update()

    # Each bone's weights go to her joint, or to the spring bone made for it or for the nearest bone above it that
    # has neither a joint of hers by name nor a spring bone.
    group_of = {}
    for bone in source.data.bones:
        walk = bone
        while springs and walk is not None and walk.name not in springs and walk.name not in named:
            walk = walk.parent
        group_of[bone.name] = springs[walk.name] if springs and walk is not None and walk.name in springs else f"joint_{joints[bone.name]:02d}"
    names = sorted(set(group_of.values()))
    onto = np.zeros((len(bones), len(names)), np.float32)
    for name, group in group_of.items():
        onto[column[name], names.index(group)] = 1.0
    _write_weights(obj, names, weights @ onto)
    return int(loose.sum())


class KAMEO_OT_fit_rig(bpy.types.Operator):
    """Make the selected rigged model part of the character with the weights it came with: its bones are matched to
    hers, it is stood and proportioned as she is, and its weights are moved onto her joints"""
    bl_idname = "kameo.fit_rig"
    bl_label = "Attach with Its Rig"
    bl_options = {"REGISTER", "UNDO"}

    place: BoolProperty(name="Stand it where she stands", default=True, description="Turn, scale and move the model onto the character. Off: it stays where you put it")
    proportions: BoolProperty(name="Match her pose and proportions", default=True, description="Move each of the model's joints onto hers, so an A-pose becomes her T-pose and its elbows and knees bend where hers do. Off: the model keeps its shape, and bends well only where its joints already sit on hers")
    springs: BoolProperty(name="Its extra bones become spring bones", default=False, description="Bones she has no joint for (hair, a skirt, a tail) are added to her skeleton as spring bones, which swing in the game. Off: what they move follows the bone they hang from")

    @classmethod
    def poll(cls, context):
        return bool(context.scene.get("kameo_model")) and _source_rig(context)[0] is not None

    def execute(self, context):
        from .blender import _character_armature

        target = _character_armature(context)
        source, meshes = _source_rig(context)
        if target is None or target == source:
            self.report({"ERROR"}, "Open a Kameo character first (File > Import > Kameo Character)")
            return {"CANCELLED"}
        if not meshes:
            self.report({"ERROR"}, "The rig moves no mesh: select the model's meshes or its armature")
            return {"CANCELLED"}
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        joints, roles, by_name = match_bones(source, target)
        transforms = fit_transforms(source, target, joints, roles, self.place and by_name, self.proportions and by_name)
        made = spring_bones(source, target, meshes, joints, roles, transforms) if self.springs and by_name else {}
        loose = sum(fit_mesh(o, source, transforms, joints, made, set(roles)) for o in meshes)
        bpy.data.objects.remove(source, do_unlink=True)
        bpy.ops.object.select_all(action="DESELECT")
        for o in meshes:
            o.select_set(True)
        context.view_layer.objects.active = meshes[0]
        result = bpy.ops.kameo.attach(mode="KEEP")
        if by_name:
            named = len(roles)
            self.report({"INFO"}, f"Attached with its own weights: {named} of its bones matched hers by name, the other {len(joints) - named} gave their weights to the bone they hang from")
        else:
            self.report({"WARNING"}, "Its bones' names say nothing about what they are, so each went to her nearest joint: stand the model exactly on her, in her pose, before attaching, and check the result")
        if made:
            self.report({"INFO"}, f"{len(made)} of its bones became spring bones: they swing in the game and stay still in Blender")
        if loose:
            self.report({"INFO"}, f"{loose} vertices had no bone: they follow the hips")
        return result


# --- a game model as a part ------------------------------------------------------------------------------------------

class KAMEO_OT_import_part(bpy.types.Operator):
    """Open one of the game's models as a part to put on the character: a .mdl, or one model of a level archive
    (.lvl), where the townsfolk and the bosses are"""
    bl_idname = "kameo.import_part"
    bl_label = "Import a Game Model as a Part"
    bl_options = {"REGISTER", "UNDO"}

    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.mdl;*.lvl", options={"HIDDEN"})
    graph: IntProperty(name="Model number", default=-1, min=-1, description="Which model of the archive (`python -m kameo_modkit info` lists them); -1 for every model in a .mdl")

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        from .gltf import export_gltf

        if self.filepath.lower().endswith(".lvl") and self.graph < 0:
            self.report({"ERROR"}, "A level archive holds dozens of models: give the number of the one you want")
            return {"CANCELLED"}
        work = tempfile.mkdtemp(prefix="kameo_part_")
        path = os.path.join(work, "part.gltf")
        export_gltf(self.filepath, path, {self.graph} if self.graph >= 0 else None)
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=path)
        new = [o for o in bpy.data.objects if o not in before]
        if not new:
            self.report({"ERROR"}, "That model has nothing to show")
            return {"CANCELLED"}
        for image in bpy.data.images:
            if image.source == "FILE" and image.filepath and os.path.dirname(bpy.path.abspath(image.filepath)).startswith(work) and not image.packed_file:
                image.pack()
        # Its joints are its own: joint_05 here is not her joint_05.
        for o in new:
            if o.type == "ARMATURE":
                for bone in o.data.bones:
                    number = _joint_number(bone.name)
                    if number is not None:
                        bone.name = f"src_{number:02d}"
            elif o.type == "MESH":
                for group in o.vertex_groups:
                    number = _joint_number(group.name)
                    if number is not None:
                        group.name = f"src_{number:02d}"
                # Its materials name its own draws, which mean nothing on the character.
                for slot in o.material_slots:
                    if slot.material:
                        slot.material.name = "part_" + slot.material.name
        self.report({"INFO"}, f"Opened as a part: place it on her and use Attach with Its Rig")
        return {"FINISHED"}


CLASSES = (KAMEO_OT_fit_rig, KAMEO_OT_import_part)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
