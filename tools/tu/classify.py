"""Sorts title-update functions into same (vanilla's code, once calls and data addresses are mapped), changed (a
vanilla twin whose code differs) and new (no twin). Writes out/tu/graft.json: {"changed": {tu: vanilla}, "new": [tu]}."""
import json, os, re

import data_map
import match

OUT = match.HERE
BRANCH = re.compile(r"^(b[a-z]*)\s")
HEX = re.compile(r"0x([0-9a-f]{8})")
STACK = re.compile(r"-?\d+\(r1\)")


_starts = {}


def map_code(t, fmap):
    """A code address through the function map, also when it points inside a function (by its offset there)."""
    if t is None:
        return None
    if t in fmap:
        return fmap[t]
    key = id(fmap)
    if key not in _starts:
        _starts[key] = sorted(fmap)
    import bisect
    starts = _starts[key]
    i = bisect.bisect_right(starts, t) - 1
    if i >= 0 and t - starts[i] < 0x4000:
        return fmap[starts[i]] + (t - starts[i])
    return ("?", t)


def canonical(insns, image_end, fmap, dmap, helper_names):
    uses = dict(data_map.address_uses(insns, image_end))
    out = []
    for i, (op, args) in enumerate(insns):
        if i in uses:
            a = uses[i]
            out.append(f"{op} @{dmap.get(a, ('?', a))}")
        elif op in ("lis", "addis"):
            out.append(f"{op} {args.split(',')[0]}")  # the address it starts is compared where it's used
        elif op == "bl":
            m = HEX.search(args)
            t = int(m.group(1), 16) if m else None
            out.append(f"bl {helper_names.get(t) or map_code(t, fmap)}")
        elif op.startswith("b") and HEX.search(args):
            out.append(op + " " + HEX.sub("L", args))  # local branches: targets differ with the code's address
        elif "(r1)" in args or (op in ("addi", "stwu") and ",r1," in args + ","):
            out.append(f"{op} {STACK.sub('#(r1)', args)}")  # a reshuffled stack frame doesn't change behaviour
        else:
            out.append(f"{op} {args}")
    return out


def main():
    vf, tf = match.parse(match.VANILLA), match.parse(match.TU)
    fmap = {int(t, 16): int(v, 16) for v, t in json.load(open(os.path.join(match.MAPS, "map.json"))).items()}
    dmap = {int(k, 16): int(v, 16) for k, v in json.load(open(os.path.join(match.MAPS, "data_map.json"))).items()}
    vid = {v: v for v in vf}
    same, changed, new = 0, {}, []
    for t, insns in tf.items():
        v = fmap.get(t)
        if v is None:
            new.append(t)
            continue
        a = canonical(insns, data_map.IMAGE_END["tu"], fmap, dmap, match.helpers[match.TU])
        b = canonical(vf[v], data_map.IMAGE_END["van"], vid, _Identity(), match.helpers[match.VANILLA])
        if a == b:
            same += 1
        else:
            changed[t] = v
    print("same", same, "changed", len(changed), "new", len(new),
          "graft insns", sum(len(tf[t]) for t in list(changed) + new))
    json.dump({"changed": {f"{t:08X}": f"{v:08X}" for t, v in sorted(changed.items())}, "new": [f"{t:08X}" for t in sorted(new)]},
              open(os.path.join(match.MAPS, "graft.json"), "w"), indent=0)


class _Identity(dict):
    def get(self, k, default=None):
        return k


if __name__ == "__main__":
    main()
