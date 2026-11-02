"""IDA script: answers out/tu/query.txt against the open database, writing out/tu/answer.txt.
Each query line is "decompile <addr>", "xrefs <addr>" (functions that reference it, by data or code), or
"disasm <addr> <count>". Run headless: idat -A -S"tools/tu/ida_query.py" <database>"""
import os

import ida_auto, ida_funcs, ida_hexrays, ida_lines, ida_name, idautils, idc

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "tu")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()


def func_name(ea):
    f = ida_funcs.get_func(ea)
    return f"{ida_name.get_name(f.start_ea)}@{f.start_ea:08X}" if f else f"?@{ea:08X}"


def xrefs(target):
    found = set()
    for x in idautils.XrefsTo(target):
        found.add(func_name(x.frm))
    # lis/addi pairs the analyser didn't turn into offsets: match the immediates.
    hi = ((target + 0x8000) >> 16) & 0xFFFF
    lo = target - (hi << 16) & 0xFFFF
    lo_signed = lo - 0x10000 if lo & 0x8000 else lo
    for seg_ea in idautils.Segments():
        if idc.get_segm_attr(seg_ea, idc.SEGATTR_TYPE) != 2:  # code
            continue
        for f in idautils.Functions(seg_ea, idc.get_segm_end(seg_ea)):
            his = False
            for ea in idautils.FuncItems(f):
                m = idc.print_insn_mnem(ea)
                if m in ("lis", "addis") and (idc.get_operand_value(ea, 1) & 0xFFFF) == hi:
                    his = True
                elif his and m in ("addi", "lwz", "lbz", "lhz", "stw", "stb", "sth", "lfs", "stfs", "lwzu", "ld", "std", "lfd", "stfd"):
                    op = idc.print_operand(ea, 1) + idc.print_operand(ea, 2)
                    if str(lo_signed) in op or hex(lo) in op.lower() or f"0x{lo:X}" in op:
                        found.add(func_name(f))
                        break
    return sorted(found)


with open(os.path.join(OUT, "query.txt")) as q, open(os.path.join(OUT, "answer.txt"), "w", encoding="utf-8") as out:
    for line in q:
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        kind, ea = parts[0], int(parts[1], 16)
        out.write(f"==== {line.strip()}\n")
        try:
            if kind == "decompile":
                out.write(str(ida_hexrays.decompile(ea)) + "\n")
            elif kind == "xrefs":
                out.write("\n".join(xrefs(ea)) + "\n")
            elif kind == "bytes":
                out.write(idc.get_bytes(ea, int(parts[2])).hex() + "\n")
                out.write(f"segment {idc.get_segm_name(ea)} {idc.get_segm_start(ea):08X}-{idc.get_segm_end(ea):08X}\n")
            elif kind == "disasm":
                for i, item in enumerate(idautils.Heads(ea, ea + 4 * int(parts[2]))):
                    out.write(f"{item:08X} {ida_lines.tag_remove(idc.generate_disasm_line(item, 0))}\n")
        except Exception as e:
            out.write(f"error: {e}\n")
idc.qexit(0)
