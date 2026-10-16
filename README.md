# Kameo: RePowered

A static recompilation of **Kameo: Elements of Power** (Xbox 360) for Windows and Linux, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

The game's PowerPC `default.xex` is translated into native x86-64 code at build time. A host layer on top replaces the Xbox 360 GPU with a native D3D12/Vulkan renderer and adds the things you would expect from a PC port: keyboard and mouse, an options menu, widescreen and high resolutions, split-screen story co-op, and downloadable content without a console.

**You must own the game.** This project contains no Kameo code, data or assets. You provide your own legally dumped copy.

## Features

- **Native renderer** built on [plume](https://github.com/renderbag/plume): D3D12 on Windows, Vulkan everywhere; any resolution, MSAA, anisotropic filtering, shadow quality, frame-rate cap, exclusive or borderless fullscreen.
- **Options menu** inside the game's own pause menu.
- **Keyboard and mouse** with rebindable keys, mouse look and a cursor in menus; controller presets and rebinding.
- **Story split-screen co-op** for a second player.
- **Downloadable content**, including the title-update packs the original game could not read.
- **Texture and movie replacement packs**.
- **Missed collectibles** left behind by a sequence break are sold back in the tribe shops for score.

## Getting a build

Kameo: RePowered is available on [Goopie](https://goopie.xyz). Stable builds are on the [releases page](https://github.com/birabittoh/KameoRePowered/releases/latest), and [nightly builds](https://nightly.link/birabittoh/KameoRePowered/workflows/ci/main?preview) come from every push to main.

Extract your game into an `assets` folder next to the executable and run `kameorepowered`.

## Building from source

The short version, assuming the SDK is unpacked and `REXGLUE_SDK` points at it:

```bash
git clone --recurse-submodules https://github.com/birabittoh/KameoRePowered.git
cd KameoRePowered
git lfs pull                                    # the recompiled shader cache
extract-xiso -d assets "Kameo - Elements of Power (USA).iso"
"$REXGLUE_SDK/bin/rexglue" codegen kameorepowered_manifest.toml
cmake --preset relwithdebinfo                   # or: release
cmake --build --preset relwithdebinfo
```

## Playing

Run the game from its build directory (`out/build/<preset>/`); it finds `assets/` on its own. Settings live in `kameo_settings.toml` next to the executable and are edited from the pause menu. Any runtime option can also be passed on the command line or kept in `kameorepowered.toml`:

```toml
user_language = 4          # French
kameo_gfx_backend = "vulkan"
```

### Default keyboard and mouse controls

| Input | Xbox 360 button |
| ----- | --------------- |
| WASD | Left stick |
| Mouse | Right stick (camera) |
| `1` / `2` / `3` | X / Y / B |
| Space | A |
| Left / right click | LT / RT |
| Q / E | LB / RB |
| Enter / Backspace | Start / Back |
| Arrow keys | D-pad |

Every binding can be changed in **Options > Controls > Key Bindings**.

### Languages

The game defaults to English. Pick another language in **Options > Gameplay**, or pass `--user_language <id>`: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean.

### Downloadable content

Copy your content packages into the save-data folder's `00000002` directory and run `python tools/extract_dlc.py`, which unpacks each one in place.

### Codes

Enter a code on any page of the pause menu (Back) to reveal a feature for good. On the keyboard X, Y, LB and RB are `1`, `2`, Q and E.

| Code | Reveals |
| ---- | ------- |
| LB RB LB RB X Y X Y | Level Select, in Options > Gameplay |
| X X Y Y LB LB RB RB | Unlock All Extras: the game's cheats, costumes, concept art and movies |
| Y X Y X RB LB RB LB | All Warriors & Upgrades (Battle Points stop while it is on, as with the game's own cheats) |
| X Y LB RB X Y LB RB | Collectibles: what this area still holds and how many are missed |

A fruit, elixir, Crystal Eye or costume left in a level the story has moved past appears after a tribe shop's own items, priced at the score its pickup awards.

### Logs and overlay keys

Logs are written to `logs/kameorepowered_N.log`. In game, F1 toggles the FPS counter, `` ` `` opens the console, F3 the debug overlay, F4 the settings overlay and F6 achievements.

## Project layout

| Path | What lives there |
| ---- | ---------------- |
| `src/app/` | The application: startup, paths, languages, window mode |
| `src/core/` | Shared plumbing: guest memory helpers, settings, the FP exception guard |
| `src/engine/` | Gameplay hooks: audio, camera, cutscenes, DLC, split-screen, cheats |
| `src/input/` | Keyboard, mouse and controller handling |
| `src/ui/` | The in-game options menu, HUD additions, name entry |
| `src/gpu/` | The native renderer: guest D3D hooks, draw capture and replay, textures, shaders, present |
| `config/` | Codegen configuration: function hints and one `hooks/<feature>.toml` per area |
| `res/` | Embedded resources: icon, cursor, the recompiled shader cache (Git LFS) |
| `tools/` | Python helpers: DLC extraction, and rebuilding the shader cache and built-in shaders |
| `thirdparty/` | plume and XenosRecomp (submodules), stb |

## Writing a hook

Guest functions are replaced at link time. Define a function with the same `sub_` name and call the original through `__imp__`:

```cpp
// SomeGuestFunction(object, value)
extern "C" REX_FUNC(sub_82123456) {
  __imp__sub_82123456(ctx, base);
}
```

To patch the middle of a function, add a `[[midasm_hook]]` to the matching `config/hooks/<feature>.toml`, declare the function in `src/core/hooks.h`, implement it, and re-run codegen:

```toml
[[midasm_hook]]
address = 0x8XXXXXXX
name = "KameoMyHook"
registers = ["r3"]
```

```cpp
void KameoMyHook(PPCRegister& r3) { r3.u32 = 1; }
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [plume](https://github.com/renderbag/plume) and [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)
- [MaxDeadBear](https://github.com/MaxDeadBear)
- [BiRabittoh](https://github.com/birabittoh)

## License

The host-side source in `src/`, the tools and the CI configuration are available under the MIT License.

The recompiled game code produced at build time contains symbols and logic from Kameo: Elements of Power and is **not** redistributable. Do not share `default.xex`, the `generated/` directory, or any built binary that links against them.
