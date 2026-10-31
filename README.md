# Kameo: RePowered

[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/8jaZQUkxZZ)

A static recompilation of **Kameo: Elements of Power** (Xbox 360) for Windows and Linux, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

The game's PowerPC `default.xex` is translated into native x86-64 code at build time. A host layer on top replaces the Xbox 360 GPU with a native D3D12/Vulkan renderer and adds the things you would expect from a PC port: keyboard and mouse, an options menu, widescreen and high resolutions, split-screen story co-op, and downloadable content without a console.

**You must own the game.** This project contains no Kameo code, data or assets. You provide your own legally dumped copy, and the launcher sets it up for you.

Everyone is welcome on the [Discord server](https://discord.gg/8jaZQUkxZZ): ask for help, report a bug, share a mod or just talk Kameo.

## Features

- **Native renderer** built on [plume](https://github.com/renderbag/plume): D3D12 on Windows, Vulkan everywhere; any resolution, MSAA, anisotropic filtering, shadow quality, frame-rate cap, exclusive or borderless fullscreen.
- **One-click setup**: the launcher unpacks your `.iso` or Games on Demand package.
- **Background shader preparation**, so the game never waits on a black screen; a progress panel shows while it runs.
- **Options menu** inside the game's own pause menu.
<!-- STEREO: -->
- **Stereoscopic 3D**: side by side or top and bottom for a 3D TV or headset, or red/cyan anaglyph on any screen, with the depth and focus distance adjustable in Options > Graphics.
- **Achievements** in the same menu, with the game's own art, and an unlock notice in the game's text box.
- **Xbox Guide** from [RexLiveless](https://rexliveless.xyz), standing in for Xbox LIVE: profiles, friends, messages, achievements, themes, online sessions and System Link. Press F10 or the controller's Guide button.
- **Optional crash reports**: if the game crashes, it asks before sending anything.
- **Keyboard and mouse** with rebindable keys, mouse look and a cursor in menus; controller presets and rebinding.
- **Story split-screen co-op** for a second player.
- **Downloadable content**, including the title-update packs the original game could not read.
- **Texture and movie replacement packs**.
- **Missed collectibles** left behind by a sequence break are sold back in the tribe shops for score.

## Getting started

**New here? Follow [Getting Started](docs/getting-started.md)**, which covers installing, the Xbox Guide, crash reports and troubleshooting.

In short, on Windows:

1. Download the Windows zip from the [releases page](https://github.com/MaxDeadBear/KameoRePowered/releases/latest) and extract it into its own folder.
2. Open `KameoLauncher` and choose your Xbox 360 disc image (`.iso`) or Games on Demand package. The launcher unpacks it for you.
3. Press Play.

On Linux, extract your game into an `assets` folder next to the executable and run `kameorepowered`.

The launcher also edits every setting, installs DLC and level mods, and switches between releases while keeping your settings, saves and game files. See [docs/launcher.md](docs/launcher.md). Kameo: RePowered is also available on [Goopie](https://goopie.xyz), and [nightly builds](https://nightly.link/MaxDeadBear/KameoRePowered/workflows/ci/main?preview) come from every push to main.

### Mods

Install mods from the launcher's Mods page: put `.kmod` files in its mods folder and switch them on. Make your own character skins and add-ons with the Blender add-on in the Windows download's `modkit` folder. See [docs/modding.md](docs/modding.md).

## Building from source

The short version, assuming the SDK is unpacked and `REXGLUE_SDK` points at it:

```bash
git clone --recurse-submodules https://github.com/MaxDeadBear/KameoRePowered.git
cd KameoRePowered
git lfs pull                                    # the recompiled shader cache
extract-xiso -d assets "Kameo - Elements of Power (USA).iso"
"$REXGLUE_SDK/bin/rexglue" codegen kameorepowered_manifest.toml
cmake --preset relwithdebinfo                   # or: release
cmake --build --preset relwithdebinfo
```

## Playing

The game reads its data from the `assets` folder next to the executable (`assets/default.xex` and the rest of the disc beside it), so give the build directory (`out/build/<preset>/`) one, or point `game_data_root` at your extracted game, in `kameorepowered.toml` or as `--game_data_root=<path>`. Settings live in `kameo_settings.toml` next to the executable and are edited from the pause menu. Any runtime option can also be passed on the command line or kept in `kameorepowered.toml`:

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

Logs are written to `logs/kameorepowered_N.log`. In game, F1 toggles the FPS counter, F4 opens the Options pages and F6 the Achievements page (both in the game's own menu), F10 opens the Xbox Guide, `` ` `` opens the console and F3 the debug overlay.

On Windows, minimizing the game reduces rendering to at most 15 FPS to save background GPU work. Restoring the window resumes your selected frame rate, including uncapped. Frame timing logs include GPU time and the number of MSAA resolves performed and reused per frame.

### Xbox Guide

The Guide comes from [RexLiveless](https://github.com/SolarCookies/RexLiveless) and uses its server at rexliveless.xyz. Press F10 or the controller's Guide button to open it, and choose Play Offline on the sign-in screen to skip making a profile. While it is on, the game connects to that server at startup, even if you never sign in, to fetch the lists of gamerpics and themes. To turn it off, set `kameo_rexliveless = false` in `kameorepowered.toml`. See [Getting Started](docs/getting-started.md#the-xbox-guide-rexliveless).

### Crash reports

Crash reports are completely optional. Most crashes never reach a bug report, so a report is how the developers find out what went wrong and fix it.

On Windows, when a release build crashes, a window asks whether to send a report through [Sentry](https://sentry.io). Nothing is sent unless you choose **Send Report**, and the game sends nothing to Sentry at any other time. Sentry doesn't store your IP address or location. To stop being asked, turn off **Offer to Send Crash Reports** on the launcher's Play page or set `crash_reports = false` in `kameo_settings.toml`; it takes effect the next time the game starts. Linux builds never send reports.

A report holds a stack-only minidump (the crash's call stacks), the game's log from that session, the last warnings and errors, the release, your OS version and a random ID for the install. The log and the minidump can show file paths, including your Windows user name if the game is installed under your user folder. The call stacks hold small pieces of the game's memory, which could rarely include your RexLiveless sign-in if it crashes while using it.

The game also writes `crash_*.txt` and `crash_*.dmp` to `logs/`, and these stay on your PC. For a bug report, attach the log and the `.txt`, but don't post the `.dmp` publicly.

## Project layout

| Path | What lives there |
| ---- | ---------------- |
| `src/app/` | The application: startup, paths, languages, window mode |
| `src/core/` | Shared plumbing: guest memory helpers, settings, the FP exception guard |
| `src/engine/` | Gameplay hooks: audio, camera, cutscenes, DLC, split-screen, cheats |
| `src/input/` | Keyboard, mouse and controller handling |
| `src/ui/` | The in-game options menu, HUD additions, name entry, the loading panel |
| `src/launcher/` | The Windows launcher: settings, game setup, DLC, mods and build switching |
| `src/crash_dialog/` | The window that asks before a crash report is sent |
| `src/gpu/` | The native renderer: guest D3D hooks, draw capture and replay, textures, shaders, present |
| `config/` | Codegen configuration: function hints and one `hooks/<feature>.toml` per area |
| `res/` | Embedded resources: icon, cursor, the recompiled shader cache (Git LFS) |
| `tools/` | Python helpers: DLC extraction, and rebuilding the shader cache and built-in shaders |
| `thirdparty/` | plume, XenosRecomp, RexLiveless and SentryWork (submodules), stb |
| `docs/` | [Getting Started](docs/getting-started.md), the [launcher](docs/launcher.md) and [modding](docs/modding.md) |

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
- [RexLiveless](https://github.com/SolarCookies/RexLiveless) by SolarCookies, for the Xbox Guide
- [SentryWork](https://github.com/masterspike52/SentryWork) by masterspike52, for crash reports
- [ReBlue](https://github.com/zolaware/reblue), whose approach to unpacking discs and compiling pipelines in the background this project follows
- [MaxDeadBear](https://github.com/MaxDeadBear)
- [BiRabittoh](https://github.com/birabittoh)

## License

The host-side source in `src/`, the tools and the CI configuration are available under the MIT License.

The recompiled game code produced at build time contains symbols and logic from Kameo: Elements of Power and is **not** redistributable. Do not share `default.xex`, the `generated/` directory, or any built binary that links against them.
