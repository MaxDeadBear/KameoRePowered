"""Kameo modding toolkit: reads and writes the game's CAFF asset archives.

Also a Blender add-on (blender.py): install this folder, zipped, from Edit > Preferences > Add-ons > Install. Outside
Blender it is a command line tool, python -m kameo_modkit.
"""

bl_info = {
    "name": "Kameo Modkit",
    "author": "Kameo RePowered",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "File > Import > Kameo Character, File > Export > Kameo Mod, 3D View > Sidebar > Kameo",
    "description": "Edit Kameo: Elements of Power characters and save them as mods for Kameo RePowered's launcher",
    "category": "Import-Export",
}


def register():
    from . import blender

    blender.register()


def unregister():
    from . import blender

    blender.unregister()
