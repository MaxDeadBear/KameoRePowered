"""Flags our hooks that may read game objects at offsets the title update moved: for each function we reference that
changed, collects the field offsets its aligned loads and stores moved (vanilla -> update), then reports source files
that mention such a vanilla offset next to that function's name or in the same file. A list to read, not a verdict."""
import json, os, re
from collections import defaultdict

import classify
import data_map
import match

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUB = re.compile(r"\bsub_([0-9A-F]{8})\b")
DFORM = re.compile(r"(-?\d+)\((r\d+)\)")


def moved_offsets(vi, ti):
    moved = defaultdict(set)
    for a, b in data_map.aligned(vi, ti):
        (vo, va), (to, ta) = vi[a], ti[b]
        if vo != to or "(r1)" in va:
            continue
        mv, mt = DFORM.search(va), DFORM.search(ta)
        if mv and mt and mv.group(1) != mt.group(1):
            moved[int(mv.group(1))].add(int(mt.group(1)))
    return moved


def main():
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    graft = json.load(open(os.path.join(match.MAPS, "graft.json")))
    changed = {int(v, 16) for v in graft["changed"].values()}
    names = json.load(open(os.path.join(match.HERE, "vanilla_names.json")))["functions"]
    files = defaultdict(set)
    for dirpath, _, fs in os.walk(os.path.join(ROOT, "src")):
        for n in fs:
            if n.endswith((".cpp", ".h")):
                p = os.path.join(dirpath, n)
                for m in SUB.finditer(open(p, encoding="utf-8", errors="replace").read()):
                    files[int(m.group(1), 16)].add(p)
    out = []
    for v in sorted(files):
        if v not in changed or v not in fmap:
            continue
        moved = moved_offsets(vf[v], tf[fmap[v]])
        if not moved:
            continue
        for p in sorted(files[v]):
            text = open(p, encoding="utf-8", errors="replace").read()
            hits = [f"{o}->{sorted(moved[o])}" for o in sorted(moved) if abs(o) >= 4 and re.search(rf"(\+\s*{o}\b|\b{o}\)|kOffset\w*\s*=\s*{o}\b|= {o};)", text)]
            if hits:
                out.append(f"{os.path.relpath(p, ROOT)}: {names.get(f'{v:08X}', f'sub_{v:08X}')} moved {', '.join(hits)}")
    open(os.path.join(match.HERE, "review_offsets.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
