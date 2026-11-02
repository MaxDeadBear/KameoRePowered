"""Translates our vanilla-addressed source and codegen config for the title-update build:

- config/hooks/*.toml -> out/tu/config/hooks/*.toml: each midasm hook moves to the matching instruction of the twin
  function (by aligning the two functions' instructions), with its registers renamed the way the twin allocated them;
- src/ -> out/tu/src/: sub_XXXXXXXX names and 0x82XXXXXX addresses become the title update's;
- overrides.toml (beside this script) settles what the maps can't, by hand: [functions], [data] and [midasm] tables
  of "vanilla hex" = "tu hex" (a midasm entry may also carry registers).

Writes out/tu/translate_report.txt: every hook or address that needed a guess, or found none."""
import bisect, difflib, json, os, re, shutil, sys, tomllib

import classify
import data_map
import match

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = match.HERE
MAPS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "maps")
# Sources are translated at configure time into the title-update workspace; the hook config is committed.
DEST_SRC = os.path.join(ROOT, "generated", "tu", "src")
DEST_CONFIG = os.path.join(ROOT, "config", "tu")
REG = re.compile(r"\b([rf]\d+|cr\d)\b")
SUB = re.compile(r"\b(__imp__)?sub_([0-9A-F]{8})\b")
# Hook macros that take a function's bare hex address (KAMEO_PASS_HOOK(8227D3E0)).
HOOK_MACRO = re.compile(r"\b(KAMEO_\w*HOOK\w*)\((8[2-6][0-9A-F]{6})\b")
# An address literal, also with an integer suffix (0x82885498u).
ADDR = re.compile(r"\b0x(8[2-9ABab][0-9A-Fa-f]{6})(?=[uUlL]*\b)")


class Maps:
    def __init__(self):
        self.fmap = {int(v, 16): int(t, 16) for v, t in json.load(open(os.path.join(MAPS, "map.json"))).items()}
        self.dinv = {}
        for t, v in json.load(open(os.path.join(MAPS, "data_map.json"))).items():
            self.dinv.setdefault(int(v, 16), int(t, 16))
        # Where several title-update addresses claim one vanilla address, the one most instructions vote for.
        for v, t in json.load(open(os.path.join(MAPS, "data_inverse.json"))).items():
            self.dinv[int(v, 16)] = int(t, 16)
        graft = json.load(open(os.path.join(MAPS, "graft.json")))
        self.changed = {int(v, 16) for v in graft["changed"].values()}
        self.tinv = {t: v for v, t in self.fmap.items()}
        self.dfwd = _Inverse(self.dinv)
        self._canon = {}
        self.last_guess = False
        overrides_path = os.path.join(os.path.dirname(__file__), "overrides.toml")
        self.overrides = tomllib.load(open(overrides_path, "rb")) if os.path.exists(overrides_path) else {}
        self.report = []

    def trees(self):
        """Both generated trees, which placing midasm hooks needs (the sources only need the maps)."""
        if not hasattr(self, "vf"):
            self.vf, self.tf = match.parse(match.VANILLA), match.parse(match.TU)
            self.vstarts = sorted(self.vf)

    def override(self, table, a):
        entry = self.overrides.get(table, {}).get(f"{a:08X}")
        if entry is None:
            return None
        return entry if isinstance(entry, dict) else {"address": entry}

    def containing(self, a):
        i = bisect.bisect_right(self.vstarts, a) - 1
        return self.vstarts[i] if i >= 0 else None

    def code(self, a):
        o = self.override("functions", a)
        if o:
            return int(o["address"], 16)
        if a in self.fmap:
            return self.fmap[a]
        if not hasattr(self, "vf"):
            return None
        f = self.containing(a)
        if f is not None and f in self.fmap and a - f < 4 * len(self.vf[f]):
            t = self.instruction(f, a)
            if t is not None:
                return t
        return None

    def data(self, a):
        o = self.override("data", a)
        if o:
            return int(o["address"], 16)
        if a in self.dinv:
            return self.dinv[a]
        # Inside an array or struct whose start is mapped: the nearest mapped addresses on both sides agree on the shift.
        if not hasattr(self, "dkeys"):
            self.dkeys = sorted(self.dinv)
        i = bisect.bisect_right(self.dkeys, a)
        if 0 < i < len(self.dkeys):
            lo, hi = self.dkeys[i - 1], self.dkeys[i]
            if a - lo <= 0x400 and self.dinv[lo] - lo == self.dinv[hi] - hi:
                return a + self.dinv[lo] - lo
        return None

    def canon(self, which, f):
        key = (which, f)
        if key not in self._canon:
            if which == "van":
                c = classify.canonical(self.vf[f], data_map.IMAGE_END["van"], {x: x for x in ()} or _Self(), classify._Identity(), match.helpers[match.VANILLA])
            else:
                c = classify.canonical(self.tf[f], data_map.IMAGE_END["tu"], self.tinv, self.dfwd, match.helpers[match.TU])
            self._canon[key] = [REG.sub("R", x) for x in c]
        return self._canon[key]

    def instruction(self, f, a):
        """The title-update address of vanilla instruction a in function f: where the twin has the same run of
        instructions around it (calls and data mapped, registers masked), else by aligning the two functions."""
        t = self.fmap[f]
        self.last_guess = False
        if a == f:
            return t
        k = (a - f) // 4
        vc, tc = self.canon("van", f), self.canon("tu", t)
        for before, after in ((4, 4), (3, 3), (6, 1), (1, 6), (2, 2), (4, 0), (0, 4)):
            lo, hi = k - before, k + after + 1
            if lo < 0 or hi > len(vc):
                continue
            window = vc[lo:hi]
            hits = [j for j in range(len(tc) - len(window) + 1) if tc[j:j + len(window)] == window]
            if len(hits) == 1:
                return t + 4 * (hits[0] + before)
        pairs = dict(data_map.aligned(self.vf[f], self.tf[t]))
        if k in pairs:
            self.last_guess = True
            return t + 4 * pairs[k]
        return None

    def registers_at(self, f, a, t_addr, regs, window=4):
        """Renames registers as the twin allocated them, from the instructions around the hook and its match."""
        t = self.fmap[f]
        vi, ti = self.vf[f], self.tf[t]
        k, j = (a - f) // 4, (t_addr - t) // 4
        rename = {}
        for d in range(-window, window + 1):
            if 0 <= k + d < len(vi) and 0 <= j + d < len(ti) and vi[k + d][0] == ti[j + d][0]:
                for x, y in zip(REG.findall(vi[k + d][1]), REG.findall(ti[j + d][1])):
                    rename.setdefault(x, y)
        return [rename.get(r, r) for r in regs], rename

    def registers(self, f, a, regs, window=4):
        """Renames registers as the twin allocated them, from the aligned instructions around the hook."""
        t = self.fmap[f]
        vi, ti = self.vf[f], self.tf[t]
        k = (a - f) // 4
        pairs = dict(data_map.aligned(vi, ti))
        rename = {}
        for j in range(max(0, k - window), min(len(vi), k + window + 1)):
            if j in pairs and vi[j][0] == ti[pairs[j]][0]:
                for x, y in zip(REG.findall(vi[j][1]), REG.findall(ti[pairs[j]][1])):
                    rename.setdefault(x, y)
        return [rename.get(r, r) for r in regs], rename

    def context_same(self, f, a, window=3):
        t = self.fmap[f]
        k = (a - f) // 4
        pairs = dict(data_map.aligned(self.vf[f], self.tf[t]))
        if k not in pairs:
            return False
        b = pairs[k]
        vc = classify.canonical(self.vf[f][max(0, k - window):k + window + 1], data_map.IMAGE_END["van"], {x: x for x in self.vf}, classify._Identity(), match.helpers[match.VANILLA])
        tc = classify.canonical(self.tf[t][max(0, b - window):b + window + 1], data_map.IMAGE_END["tu"], {y: x for x, y in self.fmap.items()}, {v: v for v in []} or _Inverse(self.dinv), match.helpers[match.TU])
        return [re.sub(REG, "R", x) for x in vc] == [re.sub(REG, "R", x) for x in tc]


class _Self(dict):
    def get(self, k, default=None):
        return k


class _Inverse(dict):
    def __init__(self, dinv):
        super().__init__({t: v for v, t in dinv.items()})

    def get(self, k, default=None):
        return super().get(k, ("?", k))


def translate_config(maps):
    maps.trees()
    shutil.rmtree(os.path.join(DEST_CONFIG, "hooks"), ignore_errors=True)
    os.makedirs(os.path.join(DEST_CONFIG, "hooks"), exist_ok=True)
    for name in sorted(os.listdir(os.path.join(ROOT, "config", "hooks"))):
        text = open(os.path.join(ROOT, "config", "hooks", name), encoding="utf-8").read()
        data = tomllib.loads(text)
        out = [f"# Translated from config/hooks/{name} for the title-update build by tools/tu/translate.py.\n"]
        for hook in data.get("midasm_hook", []):
            a = hook["address"]
            o = maps.override("midasm", a)
            f = maps.containing(a)
            if o and o.get("drop"):
                maps.report.append(f"midasm {hook['name']} {a:08X}: dropped by override ({o.get('why', '')})")
                out.append(f"# DROPPED {hook['name']} at {a:08X}: {o.get('why', '')}\n\n")
                continue
            if o:
                t, regs, note = int(o["address"], 16), o.get("registers", hook.get("registers")), "override"
            elif f in maps.fmap and (t := maps.instruction(f, a)) is not None:
                regs, rename = maps.registers_at(f, a, t, hook.get("registers", []))
                note = "GUESS: aligned, no matching run of instructions" if maps.last_guess else ("same" if f not in maps.changed else "matching run found, function changed")
                if note != "same" or regs != hook.get("registers", []):
                    maps.report.append(f"midasm {hook['name']} {a:08X} -> {t:08X} regs {hook.get('registers', [])} -> {regs}: {note}")
            else:
                maps.report.append(f"midasm {hook['name']} {a:08X}: NO MATCH (function {f:08X} {'unmatched' if f not in maps.fmap else ''})")
                out.append(f"# DROPPED {hook['name']} at {a:08X}: no title-update match\n\n")
                continue
            out.append("[[midasm_hook]]\n")
            out.append(f"address = 0x{t:08X}\n")
            if o:
                hook = {**hook, **{k: v for k, v in o.items() if k not in ("address", "registers", "why") and not k.startswith("jump_address")}}
            for key, value in hook.items():
                if key == "address":
                    continue
                if key == "registers":
                    value = regs
                if key.startswith("jump_address"):
                    if o and key in o:
                        jt = int(o[key], 16)
                    elif value == a + 4:
                        jt = t + 4  # skipping the hooked instruction carries over as is
                    else:
                        jt = maps.code(value)
                    if jt is None:
                        maps.report.append(f"midasm {hook['name']} {key} {value:08X}: NO MATCH")
                        jt = 0
                    out.append(f"{key} = 0x{jt:08X}\n")
                    continue
                out.append(f"{key} = {json.dumps(value)}\n")
            out.append("\n")
        if "rexcrt" in data:
            out.append("[rexcrt]\n")
            for key, value in data["rexcrt"].items():
                t = maps.code(value)
                if t is None:
                    maps.report.append(f"rexcrt {key} {value:08X}: NO MATCH")
                    continue
                out.append(f"{key} = 0x{t:08X}\n")
        open(os.path.join(DEST_CONFIG, "hooks", name), "w", encoding="utf-8").write("".join(out))


def write_if_changed(dest, data):
    """Writes only what changed, so a rebuild recompiles only the sources that were edited."""
    if os.path.exists(dest) and open(dest, "rb").read() == data:
        return
    open(dest, "wb").write(data)


def translate_sources(maps):
    misses = {}
    written = set()
    for dirpath, _, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, os.path.join(ROOT, "src"))
            dest = os.path.join(DEST_SRC, rel)
            written.add(os.path.normcase(dest))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if not name.endswith((".cpp", ".h", ".inc", ".hlsl", ".hlsli")):
                write_if_changed(dest, open(path, "rb").read())
                continue
            text = open(path, encoding="utf-8", errors="surrogateescape").read()

            def sub(m):
                a = int(m.group(2), 16)
                t = maps.code(a)
                if t is None:
                    misses.setdefault((rel, f"sub_{a:08X}"), 0)
                    return m.group(0)
                if a in maps.changed:
                    misses.setdefault((rel, f"sub_{a:08X} -> sub_{t:08X} (changed: review)"), 0)
                return f"{m.group(1) or ''}sub_{t:08X}"

            def addr(m):
                a = int(m.group(1), 16)
                t = maps.code(a) if 0x820B0000 <= a < 0x826E0000 else maps.data(a)
                if t is None:
                    misses.setdefault((rel, f"0x{a:08X}"), 0)
                    return m.group(0)
                return f"0x{t:08X}"

            def macro(m):
                a = int(m.group(2), 16)
                t = maps.code(a)
                if t is None:
                    misses.setdefault((rel, f"{m.group(1)}({a:08X})"), 0)
                    return m.group(0)
                return f"{m.group(1)}({t:08X}"

            # Code under #if KAMEO_TU already speaks title-update addresses, and code under #if !KAMEO_TU is vanilla's
            # alone: only the shared lines are translated.
            lines, stack = [], []
            for line in text.splitlines(keepends=True):
                d = line.strip()
                if d.startswith("#if"):
                    cond = d.split(None, 1)[1].strip() if " " in d else ""
                    stack.append("tu" if cond in ("KAMEO_TU", "defined(KAMEO_TU)") else "vanilla" if cond in ("!KAMEO_TU", "!defined(KAMEO_TU)") else "other")
                elif d.startswith("#else") and stack:
                    stack[-1] = {"tu": "vanilla", "vanilla": "tu"}.get(stack[-1], "other")
                elif d.startswith("#endif") and stack:
                    stack.pop()
                if any(s in ("tu", "vanilla") for s in stack):
                    lines.append(line)
                else:
                    lines.append(HOOK_MACRO.sub(macro, ADDR.sub(addr, SUB.sub(sub, line))))
            write_if_changed(dest, "".join(lines).encode("utf-8", errors="surrogateescape"))
    # Files deleted from src/ go from the translated tree too.
    for dirpath, _, files in os.walk(DEST_SRC):
        for name in files:
            dest = os.path.join(dirpath, name)
            if os.path.normcase(dest) not in written:
                os.remove(dest)
    for (rel, what) in sorted(misses):
        maps.report.append(f"src {rel}: {what}" + ("" if "changed" in what else " NOT TRANSLATED"))


def main() -> int:
    maps = Maps()
    translate_config(maps)
    translate_sources(maps)
    open(os.path.join(OUT, "translate_report.txt"), "w").write("\n".join(maps.report) + "\n")
    bad = [r for r in maps.report if "NO MATCH" in r or "NOT TRANSLATED" in r or "GUESS" in r]
    print(f"report: {len(maps.report)} notes, {len(bad)} need attention (out/tu/translate_report.txt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
