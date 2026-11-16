"""What Kameo's joints are, for the tools that fit other models to her: which joint is the hips, a forearm, a finger.

The game's joints have numbers and no names. These are Kameo's 81 (every costume of hers, 007183BF_xx, and her
everyday body in 011C2CC3.lvl share the skeleton), read off their places and their chains in the bind pose:

  the body        1 hips, 5 6 11 18 the spine upwards, 24 the top of the chest (the arms hang from it; she has no
                  collar bones), 28 neck, 32 head
  an arm          27 upper arm, 33 forearm, 40 hand; from the hand five chains of three: 46 58 69 index, 47 59 70
                  middle, 48 60 71 ring, 49 61 72 little, 50 62 73 thumb. The other arm is 30 34 44 and 52..56,
                  63..67, 74..78
  a leg           3 thigh, 9 shin, 15 foot, 21 toes; the other 4 10 17 22
  hers alone      35 45 57 68 79 80 the ponytail, 26 29 the chest, 37 41 the ears, 36 38 39 42 43 the face, 12 13 16
                  20 what hangs at her hip, 7 14 19 23 25 31 the loincloth (these six, and 0 2 8 51, have no skinning
                  matrix: nothing can be weighted to them)

Joints 27, 3 and the rest of the first-named side are at +X in the game, which is the character's right; a glTF and
Blender show them at -X.
"""

from __future__ import annotations

import re

JOINTS = 81

# role -> joint. ".R" is the character's right.
ROLES = {
    "hips": 1, "spine": 5, "spine1": 6, "spine2": 11, "chest": 18, "upper_chest": 24, "neck": 28, "head": 32,
    "upper_arm.R": 27, "forearm.R": 33, "hand.R": 40,
    "index1.R": 46, "index2.R": 58, "index3.R": 69, "middle1.R": 47, "middle2.R": 59, "middle3.R": 70,
    "ring1.R": 48, "ring2.R": 60, "ring3.R": 71, "little1.R": 49, "little2.R": 61, "little3.R": 72,
    "thumb1.R": 50, "thumb2.R": 62, "thumb3.R": 73,
    "upper_arm.L": 30, "forearm.L": 34, "hand.L": 44,
    "index1.L": 52, "index2.L": 63, "index3.L": 74, "middle1.L": 53, "middle2.L": 64, "middle3.L": 75,
    "ring1.L": 54, "ring2.L": 65, "ring3.L": 76, "little1.L": 55, "little2.L": 66, "little3.L": 77,
    "thumb1.L": 56, "thumb2.L": 67, "thumb3.L": 78,
    "thigh.R": 3, "shin.R": 9, "foot.R": 15, "toe.R": 21,
    "thigh.L": 4, "shin.L": 10, "foot.L": 17, "toe.L": 22,
}

# The joints any two-legged body has: what a new body is weighted to. Her ponytail, ears and the rest are hers alone.
BODY = frozenset(ROLES.values())

PARENT = {
    0: -1, 1: 0, 2: 1, 3: 1, 4: 1, 5: 1, 6: 5, 7: 5, 8: 2, 9: 3, 10: 4, 11: 6, 12: 8, 13: 8, 14: 7, 15: 9, 16: 8, 17: 10,
    18: 11, 19: 14, 20: 13, 21: 15, 22: 17, 23: 19, 24: 18, 25: 23, 26: 24, 27: 24, 28: 24, 29: 24, 30: 24, 31: 25,
    32: 28, 33: 27, 34: 30, 35: 32, 36: 32, 37: 32, 38: 32, 39: 32, 40: 33, 41: 32, 42: 32, 43: 32, 44: 34, 45: 35,
    46: 40, 47: 40, 48: 40, 49: 40, 50: 40, 51: 44, 52: 44, 53: 44, 54: 44, 55: 44, 56: 44, 57: 45, 58: 46, 59: 47,
    60: 48, 61: 49, 62: 50, 63: 52, 64: 53, 65: 54, 66: 55, 67: 56, 68: 57, 69: 58, 70: 59, 71: 60, 72: 61, 73: 62,
    74: 63, 75: 64, 76: 65, 77: 66, 78: 67, 79: 68, 80: 79,
}


def body_joint(joint: int) -> int:
    """`joint`, or the nearest joint above it that every body has: the ponytail's head, the chest's upper chest."""
    seen = set()
    while joint not in BODY and joint in PARENT and joint not in seen:
        seen.add(joint)
        joint = PARENT[joint]
    return joint if joint in BODY else ROLES["hips"]


# --- other rigs' bone names ------------------------------------------------------------------------------------------
# A bone's name is reduced to lower-case words, its side taken out; these are the words each role goes by in the rigs
# models come with: Mixamo, Unreal's mannequin, Unity and VRM humanoids, Rigify and Blender's own naming, 3ds Max
# Biped, MMD's English names, Daz and Source.
_ALIASES = {
    "hips": ("hips", "hip", "pelvis", "root hips", "lower body", "bip pelvis"),
    "spine": ("spine", "spine0", "spine 01", "spine1 lower", "abdomen", "abdomen lower", "waist", "torso"),
    "spine1": ("spine1", "spine 02", "abdomen upper", "abdomen2", "upper body"),
    "spine2": ("spine2", "spine 03", "chest lower", "upper body2"),
    "chest": ("chest", "spine3", "spine 04", "ribcage"),
    "upper_chest": ("upper chest", "chest upper", "spine4", "spine 05", "chest2"),
    "neck": ("neck", "neck 01", "neck1", "neck lower"),
    "head": ("head",),
    "upper_arm": ("arm", "upper arm", "upperarm", "shoulder bend", "shldr bend", "arm upper"),
    "forearm": ("fore arm", "forearm", "lower arm", "lowerarm", "elbow", "forearm bend", "arm lower"),
    "hand": ("hand", "wrist"),
    "thigh": ("up leg", "upleg", "upper leg", "upperleg", "thigh", "thigh bend", "leg upper"),
    "shin": ("leg", "lower leg", "lowerleg", "calf", "shin", "knee", "leg lower"),
    "foot": ("foot", "ankle"),
    "toe": ("toe base", "toebase", "toes", "toe", "ball", "toe0"),
}
_FINGERS = {"thumb": "thumb", "index": "index", "pointer": "index", "middle": "middle", "ring": "ring", "pinky": "little", "little": "little", "pinkie": "little"}
# The bone between the chest and the upper arm, which Kameo lacks: it goes to the top of her chest.
_COLLAR = ("shoulder", "clavicle", "collar", "clavicule")

_SIDE_WORDS = {"left": "L", "right": "R", "l": "L", "r": "R"}


def _words(name: str) -> tuple[list[str], str]:
    """A bone name as lower-case words, and its side ("L", "R" or "")."""
    name = name.split(":")[-1]
    name = re.sub(r"^(mixamorig\d*|bip\d*|bip001|valvebiped|def|org|j_bip|j_sec)[ _.\-]*", "", name, flags=re.IGNORECASE)
    name = re.sub(r"([a-z])([A-Z])", r"\1 \2", name)       # LeftUpLeg -> Left Up Leg
    name = re.sub(r"([A-Za-z])(\d)", r"\1 \2", name)        # Spine1 -> Spine 1
    words = [w for w in re.split(r"[ _.\-]+", name.lower()) if w]
    side = ""
    kept = []
    for i, word in enumerate(words):
        if word in ("left", "right") or (word in ("l", "r") and (i == 0 or i == len(words) - 1 or len(words) <= 3)):
            side = side or _SIDE_WORDS[word]
        else:
            kept.append(word)
    return kept, side


_HELPERS = ("twist", "roll", "ik", "pole", "target", "end", "nub", "helper", "adjust", "share")


def is_helper(name: str) -> bool:
    """Whether a bone's name marks it as a rig's helper (a twist bone, an IK target, a leaf): never a part that should
    swing on its own."""
    return any(w in _HELPERS for w in _words(name)[0])


def role_of(name: str) -> str | None:
    """The role a bone's name gives it ("forearm.L", "spine1", "index2.R"), or None when it names none: a twist or
    helper bone, hair, a skirt."""
    words, side = _words(name)
    text = " ".join(words)
    if not text or any(w in _HELPERS for w in words):
        return None
    finger = next((_FINGERS[w] for w in words if w in _FINGERS), None)
    if finger and side:
        digits = [int(w) for w in words if w.isdigit()]
        segment = digits[-1] if digits else 1
        if any(w in ("proximal", "base") for w in words):
            segment = 1
        elif any(w in ("intermediate", "mid") for w in words):
            segment = 2
        elif any(w in ("distal", "tip") for w in words):
            segment = 3
        elif any(w == "metacarpal" for w in words):
            return None
        # Mixamo and Unreal number from 1, Rigify from 01, some from 0: a 4 is the fingertip's end.
        if segment == 0:
            segment = 1
        if segment > 3:
            return None
        return f"{finger}{segment}.{side}"
    if side and any(w in _COLLAR for w in words) and not any(w in ("bend", "arm") for w in words):
        return f"collar.{side}"
    squeezed = text.replace(" ", "")
    for role, names in _ALIASES.items():
        if text in names or squeezed in (n.replace(" ", "") for n in names):
            if role in ("upper_arm", "forearm", "hand", "thigh", "shin", "foot", "toe"):
                return f"{role}.{side}" if side else None
            return role if not side else None
    return None


def joint_of_role(role: str) -> int | None:
    if role.startswith("collar."):
        return ROLES["upper_chest"]
    return ROLES.get(role)
