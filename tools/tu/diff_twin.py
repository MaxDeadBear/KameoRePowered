"""Shows how a hooked function's title-update twin differs, after mapping calls and data addresses and masking
registers and stack offsets: the lines only one side has. Usage: diff_twin.py 824D27E0 [...] [--context 0]"""
import difflib, json, os, re, sys

import classify
import data_map
import match

REG = re.compile(r"\b([rf]\d+|cr\d)\b")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    tinv = {t: v for v, t in fmap.items()}
    dmap = {int(k, 16): int(v, 16) for k, v in json.load(open(os.path.join(match.MAPS, "data_map.json"))).items()}
    names = json.load(open(os.path.join(match.HERE, "vanilla_names.json")))["functions"]
    for a in args:
        v = int(a, 16)
        t = fmap.get(v)
        if t is None:
            print(f"== {a}: no twin")
            continue
        vc = classify.canonical(vf[v], data_map.IMAGE_END["van"], {x: x for x in vf}, classify._Identity(), match.helpers[match.VANILLA])
        tc = classify.canonical(tf[t], data_map.IMAGE_END["tu"], tinv, dmap, match.helpers[match.TU])
        vc = [REG.sub("R", x) for x in vc]
        tc = [REG.sub("R", x) for x in tc]
        diff = [d for d in difflib.unified_diff(vc, tc, lineterm="", n=0) if d[:1] in "+-" and not d.startswith(("+++", "---"))]
        print(f"== {a} {names.get(a.upper(), '')} -> {t:08X}: {len(vf[v])} vs {len(tf[t])} instructions, {len(diff)} differing lines")
        for d in diff[:40]:
            print("   ", d)


if __name__ == "__main__":
    main()
