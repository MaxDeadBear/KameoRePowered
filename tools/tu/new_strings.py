"""Prints ASCII strings (6+ chars) in the title-update image that the vanilla image lacks, with their addresses."""
import os, re

HERE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "out", "tu"))
van = open(os.path.join(HERE, "vanilla_image.bin"), "rb").read()
tu = open(os.path.join(HERE, "tu_image.bin"), "rb").read()
old = set(m.group() for m in re.finditer(rb"[\x20-\x7e]{6,}", van))
for m in re.finditer(rb"[\x20-\x7e]{6,}", tu):
    if m.group() not in old:
        print(f"{0x82000000 + m.start():08X} {m.group().decode()}")
