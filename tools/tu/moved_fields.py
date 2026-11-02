"""For every changed function our sources hook or call, the object field offsets its title-update twin reads at a
different offset (aligned same-instruction loads and stores, off any register but the stack's), leaving out the D3D
device's known +16 past 0x34D8. Pair each with the offsets the source file uses to see whether a hook needs fixing."""
import json, os, re
from collections import Counter, defaultdict

import data_map
import match

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUB = re.compile(r"\bsub_([0-9A-F]{8})\b")
DFORM = re.compile(r"(-?\d+)\((r\d+)\)")


def main():
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    changed = {int(v, 16) for v in json.load(open(os.path.join(match.MAPS, "graft.json")))["changed"].values()}
    names = json.load(open(os.path.join(match.HERE, "vanilla_names.json")))["functions"]
    users = defaultdict(set)
    for dirpath, _, files in os.walk(os.path.join(ROOT, "src")):
        for n in files:
            if n.endswith((".cpp", ".h")):
                p = os.path.join(dirpath, n)
                for m in SUB.finditer(open(p, encoding="utf-8", errors="replace").read()):
                    users[int(m.group(1), 16)].add(os.path.relpath(p, ROOT))
    for v in sorted(users):
        if v not in changed or v not in fmap:
            continue
        t = fmap[v]
        moved = Counter()
        same = Counter()
        for a, b in data_map.aligned(vf[v], tf[t]):
            (vo, va), (to, ta) = vf[v][a], tf[t][b]
            mv, mt = DFORM.search(va), DFORM.search(ta)
            if vo != to or not mv or not mt or mv.group(2) == "r1":
                continue
            o, n = int(mv.group(1)), int(mt.group(1))
            if o == n:
                same[o] += 1
            elif not (o >= 0x34D8 and n - o == 16) and abs(o) < 0x8000 and abs(n) < 0x8000:
                moved[(o, n)] += 1
        # An offset also read unchanged elsewhere in the function is alignment noise, not a move.
        real = sorted((o, n, c) for (o, n), c in moved.items() if same[o] == 0 and c >= 1 and 0 <= o < 0x2000)
        if real:
            print(f"{v:08X} {names.get(f'{v:08X}', '')} ({', '.join(sorted(users[v]))}): " + ", ".join(f"{o}->{n}" + (f" x{c}" if c > 1 else "") for o, n, c in real))


if __name__ == "__main__":
    main()
