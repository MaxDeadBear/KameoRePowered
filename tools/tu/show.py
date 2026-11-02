"""Prints the instructions around addresses, from the generated code: v:<hex> for vanilla, t:<hex> for the title
update. Usage: show.py v:820B5228 t:820B5F10 [--n 5]"""
import bisect, sys

import match

n = 5
if "--n" in sys.argv:
    n = int(sys.argv[sys.argv.index("--n") + 1])
trees = {}
for arg in sys.argv[1:]:
    if ":" not in arg:
        continue
    side, hexaddr = arg.split(":")
    folder = match.VANILLA if side == "v" else match.TU
    if side not in trees:
        trees[side] = match.parse(folder)
        trees[side + "s"] = sorted(trees[side])
    funcs, starts = trees[side], trees[side + "s"]
    a = int(hexaddr, 16)
    f = starts[bisect.bisect_right(starts, a) - 1]
    k = (a - f) // 4
    print(f"== {arg} in {f:08X}+{a - f:X}")
    for i in range(max(0, k - n), min(len(funcs[f]), k + n + 1)):
        op, args = funcs[f][i]
        print(f"{'>' if i == k else ' '} {f + 4 * i:08X} {op} {args}")
