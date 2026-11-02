"""Collects every string our code shows through Tr or TrFormat (src/core/localization.h) and brings the language files
in res/lang up to date with them.

    python tools/lang/extract.py

res/lang/strings.txt lists the English with where each is used. Each language file keeps the translations it has, gains
an empty entry for every new string, and loses the strings the code no longer asks for. Fill in an entry by putting the
translation on the line under its English; an entry left empty shows in English. Run with --check to only report how
complete each language is.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LANG = os.path.join(ROOT, "res", "lang")
SOURCES = [os.path.join(ROOT, "src")]
# The code each language file is named by, as src/core/localization.cpp has them, and its name for the header.
LANGUAGES = OrderedDict([("ja", "Japanese"), ("de", "German"), ("fr", "French"), ("es", "Spanish"), ("it", "Italian"), ("ko", "Korean"), ("zh-Hant", "Traditional Chinese")])

# Tr(u"..."), Tr("..."), TrFormat(u"...", ...), TR_NOOP(u"..."): the first argument, one literal or several written
# side by side.
CALL = re.compile(r"\b(?:Tr|TrFormat|TR_NOOP)\(\s*((?:u8?|U|L)?\"(?:[^\"\\]|\\.)*\"(?:\s*(?:u8?|U|L)?\"(?:[^\"\\]|\\.)*\")*)")
LITERAL = re.compile(r"(?:u8?|U|L)?\"((?:[^\"\\]|\\.)*)\"")


def unescape_cpp(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            n = text[i + 1]
            if n == "n":
                out.append("\n")
            elif n == "t":
                out.append("\t")
            elif n in "\"'\\?":
                out.append(n)
            elif n == "u" and i + 5 < len(text):
                out.append(chr(int(text[i + 2:i + 6], 16)))
                i += 4
            elif n == "x":
                j = i + 2
                while j < len(text) and text[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(chr(int(text[i + 2:j], 16)))
                i = j - 2
            else:
                out.append(n)
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def escape_line(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\n", "\\n")


def unescape_line(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text) and text[i + 1] in "n\\":
            out.append("\n" if text[i + 1] == "n" else "\\")
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def collect() -> "OrderedDict[str, list[str]]":
    found: "OrderedDict[str, list[str]]" = OrderedDict()
    for root in SOURCES:
        for folder, _, files in sorted(os.walk(root)):
            for name in sorted(files):
                if not name.endswith((".cpp", ".h")) or name in ("localization.cpp", "localization.h"):
                    continue
                path = os.path.join(folder, name)
                text = open(path, encoding="utf-8", errors="replace").read()
                for match in CALL.finditer(text):
                    english = "".join(unescape_cpp(m.group(1)) for m in LITERAL.finditer(match.group(1)))
                    if not english.strip():
                        continue
                    line = text.count("\n", 0, match.start()) + 1
                    found.setdefault(english, []).append(f"{os.path.relpath(path, ROOT).replace(os.sep, '/')}:{line}")
    return found


def read_language(path: str) -> dict[str, str]:
    entries: dict[str, str] = {}
    if not os.path.exists(path):
        return entries
    english = None
    for raw in open(path, encoding="utf-8-sig").read().split("\n"):
        line = raw.rstrip("\r")
        if not line:
            english = None
            continue
        if line.startswith("#") and english is None:
            continue
        if english is None:
            english = unescape_line(line)
        else:
            entries[english] = unescape_line(line)
            english = None
    return entries


def write_language(path: str, name: str, strings, entries: dict[str, str]) -> tuple[int, int]:
    done = 0
    lines = [f"# {name} for Kameo: RePowered's own text: the English, the {name} under it, then a blank line.",
             "# An entry with nothing under its English shows in English. \\n is a line break. Keep {0}, {1}... and",
             "# {font tags} as they are; they're filled in or read by the game.", ""]
    for english, places in strings.items():
        translation = entries.get(english, "")
        done += bool(translation)
        lines.append(f"# {places[0]}" + (f" (+{len(places) - 1})" if len(places) > 1 else ""))
        lines.append(escape_line(english))
        lines.append(escape_line(translation))
        lines.append("")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    return done, len(strings)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="only report how complete each language is")
    arguments = parser.parse_args()
    strings = collect()
    os.makedirs(LANG, exist_ok=True)
    if not arguments.check:
        with open(os.path.join(LANG, "strings.txt"), "w", encoding="utf-8", newline="\n") as out:
            out.write("# Every string our code shows through Tr, with where it's used; written by tools/lang/extract.py.\n\n")
            for english, places in strings.items():
                out.write(f"# {', '.join(places)}\n{escape_line(english)}\n\n")
    for code, name in LANGUAGES.items():
        path = os.path.join(LANG, f"{code}.txt")
        entries = read_language(path)
        if arguments.check:
            done = sum(1 for english in strings if entries.get(english))
        else:
            done, _ = write_language(path, name, strings, entries)
        print(f"{code}: {done}/{len(strings)} translated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
