"""The Blender 2.79 add-on's helper, run by a newer Blender (2.93 or later) in the background, since 2.79's Python and
Blender cannot run the modkit. Models cross between the two as FBX.

  blender -b --factory-startup --python bridge.py -- import <model.mdl> <out.fbx>
      a game model as an FBX 2.79 opens: rigged, textured (embedded), Kameo's materials and bone names kept; and
      <out>.textures.json, per image file name, the game texture it came from (blender_textures.py)
  blender -b --factory-startup --python bridge.py -- mod <scene.fbx> <model.mdl> <mods folder> <name> <author> <description> <0|1>
      the FBX 2.79 saved, as a .kmod: Kameo's own parts known by their material names (graph<g>_mesh<m>_draw<d>), and
      everything else attached to her as the add-on's Attach does (its own weights kept where it has joint_ ones)

Prints, for the 2.79 add-on to show: "NOTE: ..." lines, then "DONE: ..." or "ERROR: ...".
"""

import json
import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import bpy  # noqa: E402

from kameo_modkit import blender as kb  # noqa: E402
from kameo_modkit.importer import DRAW_NAME  # noqa: E402


def _colour_image_direct(material) -> None:
    """The colour image linked straight to Base Color. The glTF import multiplies it by the vertex colours first, and the
    FBX export only follows a direct link, so 2.79 got every material without its colour texture."""
    tree = material.node_tree if material and material.use_nodes else None
    shader = next((n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"), None) if tree else None
    if shader is None or not shader.inputs["Base Color"].is_linked:
        return
    node = shader.inputs["Base Color"].links[0].from_node
    seen = set()
    while node is not None and node.type != "TEX_IMAGE" and node.name not in seen:
        seen.add(node.name)
        linked = [i for i in node.inputs if i.is_linked and i.links[0].from_node.type in ("TEX_IMAGE", "MIX", "MIX_RGB")]
        node = linked[0].links[0].from_node if linked else None
    if node is not None and node.type == "TEX_IMAGE":
        tree.links.new(node.outputs["Color"], shader.inputs["Base Color"])


def to_fbx(model: str, out: str) -> None:
    kb._import_model(bpy.context, model)
    for material in bpy.data.materials:
        _colour_image_direct(material)
    bpy.ops.export_scene.fbx(filepath=out, path_mode="COPY", embed_textures=True, add_leaf_bones=False, bake_anim=False, use_armature_deform_only=False)
    textures = {}
    for image in bpy.data.images:
        if image.get("kameo_game"):
            entry = json.loads(image["kameo_game"])
            textures[image.name] = entry
            textures[os.path.basename(bpy.path.abspath(image.filepath_raw))] = entry
    with open(out + ".textures.json", "w", encoding="utf-8") as handle:
        json.dump(textures, handle)
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
