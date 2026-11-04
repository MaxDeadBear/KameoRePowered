# What the game's model files are called, for both add-ons. Written for Python 3.5, as the Blender 2.79 add-on loads
# this file on its own.
#
# A character model file is named by the ELF hash of the lowercase object it dresses and a skin number:
# 007183BF_01.mdl is "kameo", skin 1. The warriors' object names are the game's own (flowerboxer, spitter, armadillo,
# rubble, firedragon, watercannon); the four not found among them were named from renders and may be wrong.

import re

CHARACTERS = {
    0x007183BF: "Kameo", 0x0CE65812: "Pummel Weed", 0x0A70BAB2: "Snare", 0x037AABBF: "Major Ruin", 0x079B8925: "Rubble",
    0x0BBEAEDE: "Ash", 0x005C444C: "Deep Blue", 0x05800519: "Chilla", 0x008B5284: "Thermite", 0x08EBDF9E: "Flex", 0x056D64DE: "40 Below",
}
KAMEO_OUTFITS = {0: "Classic", 1: "Gothic", 2: "Coyote"}


def model_label(filename):
    match = re.match(r"([0-9A-Fa-f]{8})_(\d+)\.mdl$", filename)
    if not match:
        return filename
    who, number = int(match.group(1), 16), int(match.group(2))
    if who == 0x007183BF:
        return "Kameo, " + KAMEO_OUTFITS.get(number, "outfit {}".format(number))
    return "{}, skin {}".format(CHARACTERS.get(who, "Unknown character"), number)
