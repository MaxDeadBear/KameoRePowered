"""Annotates the guest addresses (as Hex-Rays prints them, negative ints or hex) in a pseudo-code file with the
strings or words they point to in the title-update image (or the vanilla one with --vanilla)."""
import os, re, struct, sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "tu")
image = open(os.path.join(OUT, "vanilla_image.bin" if "--vanilla" in sys.argv else "tu_image.bin"), "rb").read()
text = open([a for a in sys.argv[1:] if not a.startswith("--")][0], encoding="utf-8").read()
seen = set()
for m in re.finditer(r"-21\d{8}|0x8[0-9A-Fa-f]{7}|\b8[0-9A-F]{7}\b", text):
    v = int(m.group(), 0) if not m.group().startswith("8") or m.group().startswith("0x") else int(m.group(), 16)
    if v < 0:
        v += 1 << 32
    off = v - 0x82000000
    if v in seen or not 0 <= off < len(image):
        continue
    seen.add(v)
    raw = image[off:off + 64]
    s = raw.split(b"\0")[0]
    if len(s) >= 3 and all(32 <= c < 127 for c in s):
        print(f"{m.group():>12} {v:08X} \"{s.decode()}\"")
    else:
        print(f"{m.group():>12} {v:08X} words {struct.unpack('>4I', raw[:16])}")
