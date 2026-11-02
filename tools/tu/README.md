# Title-update build tools

`kameorepowered-tu` is the game as its title update (version 0.0.2.1, August 2006) patched it: Expert mode, Expert Co-Op, Rune Battle and Time Attack, and the update's fixes. The update is a full rebuild of the game, not a small patch. About 3,700 functions changed, around 1,400 are new, and common objects such as the D3D device and the wave-bank list changed layout. So it gets a build of its own instead of hooks on the original.

Our sources stay written against the original game. Building with `KAMEO_TITLE_UPDATE` (the `release-tu` preset) recompiles the patched image and compiles a copy of `src/` translated to the update's addresses.

## Pieces

| File | What it is |
| ---- | ---------- |
| `maps/map.json` | Original → update function addresses (14,133 matched). |
| `maps/data_map.json` | Update → original data addresses, from matched functions' address-forming instructions. |
| `maps/data_inverse.json` | Original → update data addresses where several update addresses claim one original: the one most instructions vote for. Without it frame time went to a neighbour nothing reads. |
| `maps/graft.json` | Which update functions changed or are new, so translation can flag hooks on changed ones. |
| `overrides.toml` | What the maps can't settle, checked by hand: CRT routines, import stubs, mid-function hooks, array elements. |
| `../../config/tu/` | The update's codegen hints (`functions.toml`) and our midasm hooks placed in its code (`hooks/`, written by `translate.py --config`). |
| `translate.py` | `--sources` writes `generated/tu/src`, the build's translated `src/`. `--config` writes `config/tu/hooks` and reports what needed a guess. |
| `prepare.py` | Sets up `generated/tu`, the build's workspace (patched executable, codegen manifest), and translates the sources. CMake runs it. |

`#if KAMEO_TU` marks code that differs between the builds. The translator leaves lines under `#if KAMEO_TU` and `#if !KAMEO_TU` alone, because they already use their own build's addresses. Examples are the device offsets past `kDeviceTail` in `src/gpu/guest_device.h` and the wave-bank node fields in `src/engine/audio_language.cpp`.

## When hooks change

- **A new `sub_` or address in `src/`:** the translator maps it. If it can't, it lists it as `NOT TRANSLATED` in `out/tu/translate_report.txt` and leaves it unchanged. Add an entry to `overrides.toml`, or guard the code with `KAMEO_TU`. Never leave an untranslated name in shared code: it could name some unrelated function in the update.
- **A hook on a function marked "changed: review" in the report:** read the twin first, because its fields may have moved. `review_offsets.py` lists hooks whose functions moved an offset our code uses.
- **A new or edited midasm hook in `config/hooks/`:** run `python tools/tu/translate.py --config`. It needs both generated trees: the original's in `generated/default`, the update's from `prepare.py --codegen`. Check every `GUESS` line, then commit `config/tu/hooks/`.

## Rebuilding the maps

Only needed if the matching itself improves. You need `assets/TU/default.xexp`, both generated trees and a relwithdebinfo build (for `rexruntimerd.dll`):

```
python tools/tu/build_images.py        # out/tu/vanilla_image.bin, tu_image.bin
python tools/tu/prepare.py --codegen   # generated/tu/generated/default
python tools/tu/match.py               # maps/map.json
python tools/tu/data_map.py            # maps/data_map.json, maps/data_inverse.json
python tools/tu/classify.py            # maps/graft.json
python tools/tu/translate.py           # config/tu/hooks, generated/tu/src
```

Reading the update's code:
- `show.py v:<addr> t:<addr>` prints the instructions around an address on either side.
- `suggest.py <vanilla addr>` ranks likely twins for an unmatched function.
- `ida_build_tu.py` builds an IDA database of the patched image named after the original's functions (load `tu_image.bin` with `-T"Binary file" -pppc -b8200000`).
- `ida_query.py` and `ida_run.py` answer xref and disassembly questions headless.

Import stubs are matched by the ordinal in their thunk's record, not by distance: the update added imports.
