"""IDA script: writes every named function of the open database to out/tu/vanilla_names.json ({hex address: name}).
Run headless on a copy of the named vanilla database: idat -A -S"tools/tu/ida_dump_names.py" kameo.i64"""
import json, os

import ida_auto, ida_name, idautils, idc

ida_auto.auto_wait()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "tu", "vanilla_names.json")
names = {}
for ea in idautils.Functions():
    name = ida_name.get_name(ea)
    if name and not name.startswith(("sub_", "nullsub_", "j_")):
        names[f"{ea:08X}"] = name
data = {}
for ea, name in idautils.Names():
    if not idc.get_func_attr(ea, idc.FUNCATTR_START) == ea and not name.startswith(("off_", "dword_", "byte_", "word_", "unk_", "loc_", "a", "flt_", "dbl_", "qword_", "stru_", "xmmword_")):
        data[f"{ea:08X}"] = name
json.dump({"functions": names, "data": data}, open(out, "w"), indent=0)
idc.qexit(0)
