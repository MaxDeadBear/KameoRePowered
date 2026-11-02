"""The guest D3D device's field offsets in vanilla vs the title update (built with a newer XDK, whose device struct
differs): every aligned load/store in a pair of D3D functions votes for vanilla offset -> update offset. Prints the
offsets whose move a clear majority agrees on, and writes them to maps/device_offsets.json for translate.py."""
import json, os, re
from collections import Counter, defaultdict

import data_map
import match

DFORM = re.compile(r"(-?\d+)\((r\d+)\)")


def device_registers(insns):
    """For each instruction, the registers holding the device pointer (r3 at entry and its mr copies) before it."""
    held, out = {"r3"}, []
    for op, args in insns:
        out.append(set(held))
        a = args.replace(" ", "").split(",")
        if op == "mr" and len(a) == 2:
            if a[1] in held:
                held.add(a[0])
            else:
                held.discard(a[0])
        elif op == "bl":
            held -= {f"r{i}" for i in range(3, 13)}
        elif a and a[0] in held and not op.startswith(("st", "cmp", "b", "mt")):
            held.discard(a[0])
    return out


def main():
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    names = json.load(open(os.path.join(match.HERE, "vanilla_names.json")))["functions"]
    votes = defaultdict(Counter)
    for v, t in fmap.items():
        name = names.get(f"{v:08X}", "")
        # D3DDevice_* take the device as their first argument: only accesses through it vote.
        if not name.startswith("D3DDevice_"):
            continue
        vdev, tdev = device_registers(vf[v]), device_registers(tf[t])
        for a, b in data_map.aligned(vf[v], tf[t]):
            (vo, va), (to, ta) = vf[v][a], tf[t][b]
            if vo != to:
                continue
            mv, mt = DFORM.search(va), DFORM.search(ta)
            if mv and mt and mv.group(2) in vdev[a] and mt.group(2) in tdev[b]:
                votes[int(mv.group(1))][int(mt.group(1))] += 1
    table = {}
    for off in sorted(votes):
        (best, n), total = votes[off].most_common(1)[0], sum(votes[off].values())
        if n * 4 >= total * 3:
            table[off] = best
    moved = {o: t for o, t in table.items() if o != t}
    print(f"{len(table)} device offsets agreed, {len(moved)} moved")
    shifts = Counter(t - o for o, t in moved.items())
    print("shifts", shifts.most_common(8))
    # Ranges: runs of offsets sharing one shift.
    runs, last = [], None
    for o in sorted(table):
        s = table[o] - o
        if last and last[2] == s:
            last[1] = o
        else:
            last = [o, o, s]
            runs.append(last)
    for lo, hi, s in runs:
        print(f"  {lo:6}-{hi:6} shift {s:+}")
    json.dump({str(o): t for o, t in table.items()}, open(os.path.join(match.MAPS, "device_offsets.json"), "w"), indent=0)


if __name__ == "__main__":
    main()
