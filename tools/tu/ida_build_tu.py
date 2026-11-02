"""IDA script for a database made from out/tu/tu_image.bin loaded raw as big-endian PowerPC at 0x82000000:
splits it into its PE sections, creates every function rexglue's codegen found (out/tu/ws/generated/tu), and names
the ones matched to vanilla (map.json) after vanilla's names (vanilla_names.json, from ida_dump_names.py).
idat -A -pppc -b8200000 -S"tools/tu/ida_build_tu.py" tu_image.bin"""
import json, os, re, struct

import ida_auto, ida_bytes, ida_funcs, ida_name, ida_segment, idc

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "tu")
BASE = 0x82000000


def sections():
    # The PE headers are little-endian, unlike the code.
    d = open(os.path.join(OUT, "tu_image.bin"), "rb").read(0x1000)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    count = struct.unpack_from("<H", d, pe + 6)[0]
    table = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(count):
        raw = d[table + 40 * i:table + 40 * i + 40]
        name = raw[:8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va = struct.unpack_from("<II", raw, 8)
        flags = struct.unpack_from("<I", raw, 36)[0]
        yield name, BASE + va, BASE + va + vsize, bool(flags & 0x20000000)


ida_auto.auto_wait()
first = ida_segment.get_first_seg()
if first and first.start_ea != BASE:
    ida_segment.rebase_program(BASE - first.start_ea, ida_segment.MSF_FIXONCE)
secs = list(sections())
print("sections", [(n, hex(s), hex(e), x) for n, s, e, x in secs])
for name, start, end, code in secs:
    seg = ida_segment.getseg(start)
    if seg and seg.start_ea < start:
        ida_segment.set_segm_end(seg.start_ea, start, ida_segment.SEGMOD_KEEP)
    ida_segment.add_segm(0, start, end, name, "CODE" if code else "DATA")
    s = ida_segment.getseg(start)
    if s:
        s.bitness = 1
        s.perm = (ida_segment.SEGPERM_EXEC | ida_segment.SEGPERM_READ) if code else (ida_segment.SEGPERM_READ | ida_segment.SEGPERM_WRITE)
        s.update()

starts = [int(m.group(1), 16) for m in re.finditer(r"\{ 0x([0-9A-F]{8}), sub_", open(os.path.join(OUT, "..", "..", "generated", "tu", "generated", "default", "kameorepowered_init.cpp")).read())]
for ea in starts:
    idc.create_insn(ea)
    ida_funcs.add_func(ea)
ida_auto.auto_wait()

mapping = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "maps", "map.json")))
names = json.load(open(os.path.join(OUT, "vanilla_names.json")))["functions"]
named = 0
for van, tu in mapping.items():
    if van in names:
        ida_name.set_name(int(tu, 16), names[van], ida_name.SN_NOWARN | ida_name.SN_NOCHECK | ida_name.SN_FORCE)
        named += 1
print("functions", len(starts), "named", named)
ida_auto.auto_wait()
idc.qexit(0)
