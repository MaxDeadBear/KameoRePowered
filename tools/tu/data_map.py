"""Pairs the title update's data addresses with vanilla's: every lis-based address a matched function pair forms at
the same aligned instruction votes for a pair. Writes out/tu/data_map.json ({tu hex: vanilla hex}) and reports
coverage of all the addresses title-update code forms.

Also importable: address_uses(insns) gives (index, address, kind) for each address an instruction list forms."""
import difflib, json, os, re
from collections import Counter, defaultdict

import match

OUT = match.HERE
IMAGE_END = {"tu": 0x82C90000, "van": 0x82BF0000}
DFORM = re.compile(r"^(r\d+|f\d+|v\d+),(-?\d+)\((r\d+)\)$")
ADDI = re.compile(r"^(r\d+),(r\d+),(-?\d+)$")
LIS = re.compile(r"^(r\d+),(-?\d+)$")
VOLATILE = {f"r{i}" for i in [0] + list(range(3, 13))}


MR = re.compile(r"^(r\d+),(r\d+)$")


def address_uses(insns, image_end):
    """Yields (index, address) for every instruction that forms an address in the image from a register holding a
    constant built with lis (and addi/ori/mr on top of it)."""
    hi = {}
    for i, (op, args) in enumerate(insns):
        a = args.replace(" ", "")
        if op == "lis":
            m = LIS.match(a)
            if m:
                hi[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
                continue
        if op in ("addi", "ori"):
            m = ADDI.match(a)
            if m and m.group(2) in hi:
                base_value = hi[m.group(2)]
                value = (base_value + int(m.group(3))) & 0xFFFFFFFF if op == "addi" else base_value | int(m.group(3))
                if 0x82000000 <= value < image_end:
                    yield i, value
                hi[m.group(1)] = value
                continue
        if op == "mr":
            m = MR.match(a)
            if m and m.group(2) in hi:
                hi[m.group(1)] = hi[m.group(2)]
                continue
        m = DFORM.match(a)
        if m and m.group(3) in hi:
            addr = (hi[m.group(3)] + int(m.group(2))) & 0xFFFFFFFF
            if 0x82000000 <= addr < image_end:
                yield i, addr
        # whatever this instruction writes stops holding a lis value
        if op.startswith("b") and op not in ("bl",):
            continue
        if op == "bl" or op == "bctrl":
            for r in VOLATILE:
                hi.pop(r, None)
            continue
        first = a.split(",")[0] if a else ""
        if not op.startswith(("st", "cmp", "mt", "tw", "dcb")) and first in hi:
            hi.pop(first, None)
        if op.endswith("u") and m:
            hi.pop(m.group(3), None)


def aligned(vi, ti):
    if len(vi) == len(ti):
        return list(zip(range(len(vi)), range(len(ti))))
    sm = difflib.SequenceMatcher(None, [o for o, _ in vi], [o for o, _ in ti], autojunk=False)
    pairs = []
    for a, b, n in sm.get_matching_blocks():
        pairs.extend((a + k, b + k) for k in range(n))
    return pairs


def main():
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    mapping = {int(k, 16): int(v, 16) for k, v in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    votes = defaultdict(Counter)
    for v, t in mapping.items():
        # A function the codegen config has since split or merged on one side has no twin to compare.
        if v not in vf or t not in tf:
            continue
        vu = dict(address_uses(vf[v], IMAGE_END["van"]))
        tu = dict(address_uses(tf[t], IMAGE_END["tu"]))
        if not vu or not tu:
            continue
        for a, b in aligned(vf[v], tf[t]):
            if a in vu and b in tu:
                votes[tu[b]][vu[a]] += 1
    dmap = {}
    for t, c in votes.items():
        (v, n), = c.most_common(1)
        if n * 2 > sum(c.values()) or len(c) == 1:
            dmap[t] = v
    # Several title-update addresses can claim the same vanilla one (a stray vote or two for a neighbour), so the way
    # back, which translate.py uses, takes the one most instructions agree on: vanilla's frameTime 82755760 drew one
    # vote for 827A9CC8 and hundreds for 827A9CD4, and the first of them in address order was taken.
    back = defaultdict(Counter)
    for t, v in dmap.items():
        back[v][t] += votes[t][v]
    inverse = {v: c.most_common(1)[0][0] for v, c in back.items()}
    json.dump({f"{v:08X}": f"{t:08X}" for v, t in sorted(inverse.items())}, open(os.path.join(match.MAPS, "data_inverse.json"), "w"), indent=0)
    # Every address title-update code forms, for coverage.
    used = Counter()
    for t, insns in tf.items():
        for _, addr in address_uses(insns, IMAGE_END["tu"]):
            used[addr] += 1
    print("pairs", len(dmap), "tu addresses formed", len(used), "covered directly", sum(1 for a in used if a in dmap))
    # Fill gaps: an address between two anchors with the same shift takes that shift.
    anchors = sorted((t, v - t) for t, v in dmap.items() if not (0x820B0000 <= t < 0x82720000))
    keys = [a for a, _ in anchors]
    import bisect
    filled = 0
    for addr in used:
        if addr in dmap or 0x820B0000 <= addr < 0x82720000:
            continue
        i = bisect.bisect_left(keys, addr)
        if 0 < i < len(keys) and anchors[i - 1][1] == anchors[i][1]:
            dmap[addr] = addr + anchors[i][1]
            filled += 1
    uncovered = [a for a in used if a not in dmap and not 0x820B0000 <= a < 0x82720000]
    print("filled between equal-shift anchors", filled, "data addresses still unmapped", len(uncovered),
          "uses", sum(used[a] for a in uncovered))
    json.dump({f"{t:08X}": f"{v:08X}" for t, v in sorted(dmap.items())}, open(os.path.join(match.MAPS, "data_map.json"), "w"), indent=0)
    with open(os.path.join(OUT, "data_unmapped.txt"), "w") as f:
        for a in sorted(uncovered):
            f.write(f"{a:08X} {used[a]}\n")


if __name__ == "__main__":
    main()
