"""Lists the characters each of the game's languages can draw in game, from the font in its language level, and checks
our translations against them.

    python tools/lang/font_chars.py --assets out/build/release/assets            # writes res/lang/fonts/<code>.txt
    python tools/lang/font_chars.py --assets out/build/release/assets --check    # reports characters translations need

The game draws text from a bitmap font holding only the characters its own text uses: a few thousand for Japanese,
Korean and Chinese. A character missing from it draws as the font's default, so our in-game text in those languages
must keep to the characters listed here (the launcher and editor draw with Windows fonts and have no such limit).
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "modkit"))
from kameo_modkit.caff import Caff  # noqa: E402
from kameo_modkit.graph import Ref, Resolver  # noqa: E402

# The language folders the game's assets have, by the code our lang files use.
FOLDERS = {"en": "English", "ja": "Japanese", "de": "German", "fr": "French", "es": "Spanish", "it": "Italian", "ko": "Korean", "zh-Hant": "TChinese"}
LANGUAGE_LEVEL = "0582CB8C.lvl"
# Where the text drawn in game with the game's font comes from.
IN_GAME = ("src/ui/", "src/engine/", "src/input/", "src/app/", "src/gpu/")
# The font's character table: 8192 four-byte slots (character, glyph) hashed as GetCharIndex probes them, its address
# at +824 in the font, and the most probes a lookup makes in the record +20 points at, at +28.
TABLE_POINTER = 824
SLOTS = 0x2000
LIMIT_RECORD = 20
LIMIT_FIELD = 28


def characters(path: str) -> set[int]:
    archive = Caff.load(path)
    resolver = Resolver(archive)
    font = next(e for e in archive.entries if resolver.type_name(e) == "font")
    start = Ref(font, 0)
    table = resolver.pointer(start, TABLE_POINTER)
    limit = resolver.u32(resolver.pointer(start, LIMIT_RECORD), LIMIT_FIELD)
    slots = resolver.bytes(table, SLOTS * 4)

    def index_of(c: int) -> int:
        probe = 1
        while probe <= limit:
            slot = ((((c >> 5) + 1) % 0x1FFF + 1) * probe + c) % 0x2000
            code, glyph = struct.unpack_from(">HH", slots, 4 * slot)
            if code == c:
                return glyph
            probe += 1
        return -1

    found = set()
    for slot in range(SLOTS):
        code = struct.unpack_from(">H", slots, 4 * slot)[0]
        if code and index_of(code) >= 0:
            found.add(code)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--assets", required=True)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import extract  # noqa: E402
    places = extract.collect()
    out = os.path.join(ROOT, "res", "lang", "fonts")
    os.makedirs(out, exist_ok=True)
    for code, folder in FOLDERS.items():
        chars = characters(os.path.join(arguments.assets, folder, LANGUAGE_LEVEL))
        if not arguments.check:
            with open(os.path.join(out, f"{code}.txt"), "w", encoding="utf-8", newline="\n") as handle:
                handle.write("".join(chr(c) for c in sorted(chars) if c >= 0x20))
        if code == "en":
            print(f"en: {len(chars)} characters")
            continue
        entries = extract.read_language(os.path.join(ROOT, "res", "lang", f"{code}.txt"))
        missing = {}
        for english, translation in entries.items():
            # Only text drawn with the game's font: the launcher, crash dialog and editor use Windows fonts.
            if not any(place.startswith(IN_GAME) for place in places.get(english, [])):
                continue
            for ch in translation:
                # U+E000 on are the controller buttons, which the game draws as icons rather than from its font.
                if ch not in "\n" and not 0xE000 <= ord(ch) <= 0xE0FF and ord(ch) not in chars:
                    missing.setdefault(ch, english)
        print(f"{code}: {len(chars)} characters; in-game translations use {len(missing)} it lacks" + (": " + "".join(sorted(missing)) if missing else ""))
        for ch, english in sorted(missing.items()):
            print(f"  {ch} in: {english[:70]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
