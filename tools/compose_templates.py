"""Writes the codegen templates RexFabric's hook slots need into generated/templates, before codegen runs.

CMake does this at configure time (thirdparty/RexFabric/cmake/rexfabric_templates.cmake), but configuring needs the
recompiled code, and codegen needs the templates: on a fresh checkout something has to write them first. This does
what RexFabric's CMake does for the two templates it changes, byte for byte, so configuring afterwards rewrites nothing.

    python tools/compose_templates.py <rexglue-sdk>/resources/templates
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAGMENTS = os.path.join(ROOT, "thirdparty", "RexFabric", "templates")
OUT = os.path.join(ROOT, "generated", "templates")
PCH_ANCHOR = b"#ifndef DECLARE_REX_FUNC"
CR_LF = bytes([13, 10])
LF = bytes([10])


def read(path: str) -> bytes:
    """A file's text with plain line feeds, as CMake reads it."""
    with open(path, "rb") as handle:
        return handle.read().replace(CR_LF, LF)


def written(text: bytes) -> bytes:
    """The bytes CMake's file(WRITE) puts on disk for `text`: on Windows it writes a line feed as CR LF."""
    return text.replace(LF, CR_LF) if os.name == "nt" else text


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    sdk = sys.argv[1]
    pch = read(os.path.join(sdk, "codegen", "pch_h.inja"))
    if pch.count(PCH_ANCHOR) != 1:
        print(f"the SDK's pch_h template has {pch.count(PCH_ANCHOR)} of the line the hook slots go before; RexFabric needs updating for this SDK")
        return 1
    at = pch.index(PCH_ANCHOR)
    composed = {
        "pch_h.inja": pch[:at] + read(os.path.join(FRAGMENTS, "pch_hooks.inja")) + pch[at:],
        "init_cpp.inja": read(os.path.join(sdk, "codegen", "init_cpp.inja")) + read(os.path.join(FRAGMENTS, "init_slots.inja")),
    }
    os.makedirs(os.path.join(OUT, "codegen"), exist_ok=True)
    for name, text in composed.items():
        path = os.path.join(OUT, "codegen", name)
        text = written(text)
        on_disk = None
        if os.path.exists(path):
            with open(path, "rb") as handle:
                on_disk = handle.read()
        if on_disk != text:
            with open(path, "wb") as handle:
                handle.write(text)
            print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
