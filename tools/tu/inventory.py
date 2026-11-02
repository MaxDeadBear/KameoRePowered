"""Lists every game function and address our source and codegen config use, with how each carries over to the title
update: same (identical code), changed (a twin whose code differs: review the hook), or missing (no twin)."""
import json, os, re
from collections import defaultdict

import match

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = match.HERE
SUB = re.compile(r"\bsub_([0-9A-F]{8})\b")
ADDR = re.compile(r"\b0x(8[2-9A-Ba-b][0-9A-Fa-f]{6})(?=[uUlL]*\b)")


def load():
    fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    dinv = {}
    for t, v in json.load(open(os.path.join(match.MAPS, "data_map.json"))).items():
        dinv.setdefault(int(v, 16), int(t, 16))
    graft = json.load(open(os.path.join(match.MAPS, "graft.json")))
    changed = {int(v, 16) for v in graft["changed"].values()}
    return fmap, dinv, changed


def scan():
    uses = defaultdict(set)
    for folder in ("src", "config"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, folder)):
            for name in files:
                if not name.endswith((".cpp", ".h", ".toml", ".inc")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT).replace("\\", "/")
                for line in open(path, encoding="utf-8", errors="replace"):
                    for m in SUB.finditer(line):
                        uses[("func", int(m.group(1), 16))].add(rel)
                    for m in ADDR.finditer(line):
                        uses[("addr", int(m.group(1), 16))].add(rel)
    return uses


def main():
    fmap, dinv, changed = load()
    uses = scan()
    rows = defaultdict(list)
    for (kind, a), files in sorted(uses.items()):
        if kind == "func" or 0x820B0000 <= a < 0x826E0000:
            if a in fmap:
                status = "changed" if a in changed else "same"
                rows[status].append((a, fmap[a], files))
            else:
                rows["missing-code"].append((a, None, files))
        else:
            if a in dinv:
                rows["data"].append((a, dinv[a], files))
            else:
                rows["missing-data"].append((a, None, files))
    with open(os.path.join(OUT, "inventory.txt"), "w") as f:
        for status in ("same", "changed", "missing-code", "data", "missing-data"):
            f.write(f"## {status} {len(rows[status])}\n")
            for a, t, files in rows[status]:
                f.write(f"{a:08X} -> {t:08X} " if t else f"{a:08X} -> ? ")
                f.write(" ".join(sorted(files)) + "\n")
            print(status, len(rows[status]))


if __name__ == "__main__":
    main()
