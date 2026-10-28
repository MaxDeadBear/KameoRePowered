"""The Blender 2.79 add-on's helper, run by a newer Blender (2.93 or later) in the background, since 2.79's Python and
Blender cannot run the modkit. Models cross between the two as FBX.

  blender -b --factory-startup --python bridge.py -- import <model.mdl> <out.fbx>
      a game model as an FBX 2.79 opens: rigged, textured (embedded), Kameo's materials and bone names kept
  blender -b --factory-startup --python bridge.py -- mod <scene.fbx> <model.mdl> <mods folder> <name> <author> <description> <0|1>
      the FBX 2.79 saved, as a .kmod: Kameo's own parts known by their material names (graph<g>_mesh<m>_draw<d>), and
      everything else attached to her as the add-on's Attach does (its own weights kept where it has joint_ ones)

Prints, for the 2.79 add-on to show: "NOTE: ..." lines, then "DONE: ..." or "ERROR: ...".
"""

import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import bpy  # noqa: E402

from kameo_modkit import blender as kb  # noqa: E402
from kameo_modkit.importer import DRAW_NAME  # noqa: E402


def to_fbx(model: str, out: str) -> None:
    kb._import_model(bpy.context, model)
    bpy.ops.export_scene.fbx(filepath=out, path_mode="COPY", embed_textures=True, add_leaf_bones=False, bake_anim=False, use_armature_deform_only=False)
    print("DONE:", out)


def to_mod(fbx: str, model: str, mods: str, name: str, author: str, description: str, enable: bool) -> None:
    context = bpy.context
    bpy.ops.import_scene.fbx(filepath=fbx)
    context.scene["kameo_model"] = model
    context.scene["kameo_label"] = kb.model_label(os.path.basename(model))
    armatures = [o for o in context.scene.objects if o.type == "ARMATURE"]
    if not armatures:
        raise kb.ModError("the scene has no armature: open the character with File > Import > Kameo Character first")
    armatures[0]["kameo_character"] = True

    # Kameo's own parts are those whose every material is one of her draws; a mesh mixing hers and others' is split.
    def kameo_material(material) -> bool:
        return material is not None and DRAW_NAME.search(material.name) is not None and "." not in material.name.split("_draw")[-1]

    for o in [o for o in context.scene.objects if o.type == "MESH"]:
        materials = [s.material for s in o.material_slots]
        if any(kameo_material(m) for m in materials) and not all(kameo_material(m) for m in materials):
            bpy.ops.object.select_all(action="DESELECT")
            o.select_set(True)
            context.view_layer.objects.active = o
            bpy.ops.object.mode_set(mode="EDIT")
            bpy.ops.mesh.separate(type="MATERIAL")
            bpy.ops.object.mode_set(mode="OBJECT")
    foreign = []
    for o in [o for o in context.scene.objects if o.type == "MESH"]:
        materials = [s.material for s in o.material_slots]
        if materials and all(kameo_material(m) for m in materials):
            o["kameo_character"] = True
        elif len(o.data.polygons):
            foreign.append(o)
    for part in foreign:
        bpy.ops.object.select_all(action="DESELECT")
        part.select_set(True)
        context.view_layer.objects.active = part
        result = bpy.ops.kameo.attach(mode="AUTO")
        if "FINISHED" not in result:
            raise kb.ModError(f"{part.name} could not be attached to the character")
    filename, notes = kb.make_mod(context, model, mods, name, author, description, enable)
    for note in notes:
        print("NOTE:", note)
    print("DONE:", filename)


def main() -> None:
    args = sys.argv[sys.argv.index("--") + 1:]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    kb.register()
    try:
        if args[0] == "import":
            to_fbx(args[1], args[2])
        elif args[0] == "mod":
            to_mod(args[1], args[2], args[3], args[4], args[5], args[6], args[7] == "1")
        else:
            raise kb.ModError(f"unknown command {args[0]}")
    except kb.ModError as error:
        print("ERROR:", error)
    except Exception as error:  # the 2.79 add-on shows this line; the trace is for a bug report
        traceback.print_exc()
        print("ERROR:", f"{type(error).__name__}: {error}")


main()
