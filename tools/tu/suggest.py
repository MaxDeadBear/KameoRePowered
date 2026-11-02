"""Ranks the title update's unmatched functions as twins of the given vanilla functions, by opcode 3-grams and the
callees they share (through map.json). Usage: suggest.py 82107D88 [8251ED98 ...]"""
import json, os, sys
from collections import Counter

import classify
import match

OUT = match.HERE


def grams(insns):
    ops = [o for o, _ in insns]
    return Counter(zip(ops, ops[1:], ops[2:]))


def callees(insns):
    return set(match.calls(insns))


def main():
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    used = set(fmap.values())
    names = json.load(open(os.path.join(OUT, "vanilla_names.json")))["functions"]
    pool = [t for t in tf if t not in used]
    pool_grams = {t: grams(tf[t]) for t in pool}
    for arg in sys.argv[1:]:
        v = int(arg, 16)
        if v not in vf:
            print(arg, "is not a vanilla function start")
            continue
        g = grams(vf[v])
        vc = {fmap.get(c) for c in callees(vf[v])} - {None}
        scored = []
        for t in pool:
            tg = pool_grams[t]
            inter = sum((g & tg).values())
            union = sum((g | tg).values()) or 1
            tc = callees(tf[t])
            share = len(vc & tc) / (len(vc | tc) or 1)
            scored.append((inter / union * 0.6 + share * 0.4, t, inter / union, share))
        scored.sort(reverse=True)
        print(f"{v:08X} {names.get(f'{v:08X}', '')} ({len(vf[v])} insns):")
        for score, t, gs, cs in scored[:4]:
            print(f"   {t:08X} score {score:.2f} (shape {gs:.2f}, callees {cs:.2f}) {len(tf[t])} insns")


if __name__ == "__main__":
    main()
