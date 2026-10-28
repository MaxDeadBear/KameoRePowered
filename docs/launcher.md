# Launcher

`KameoLauncher.exe` sits beside `kameorepowered.exe`. It edits the same
`kameo_settings.toml` the pause menu writes, rebinds keys, installs Xbox 360 DLC
packages, applies `.kmod` mods (levels and character models), downloads and switches game builds, and
starts the game. It is a separate
Windows executable (Direct3D 11 + Dear ImGui through the vendored
`thirdparty/imgui_backends`), built from `src/launcher/**/*.cpp` with the
launcher's own flags and linked against the game's runtime DLL for ImGui and STFS.
That DLL is delay-loaded from `launcher\rexruntime.dll`, a copy built with the
launcher (`runtime_loader.cpp`), so the launcher never locks the game's own
`rexruntime.dll` and keeps working whichever build is switched in. Without that
folder it falls back to the game's copy.

## Files

| File | What it does |
| --- | --- |
| `main.cpp` | `wWinMain`: command line, the console-only modes, settings load, then the window |
| `app.h` | `App` (state every page shares), `Page`, the palette, entry points |
| `window_d3d11.cpp` | Win32 window, D3D11 swap chain, WndProc (resize, DPI, min size), the frame loop |
| `launch.cpp` | Starting the game, the launcher's own preferences (`kameo_launcher.toml`) |
| `ui/theme.cpp` | ImGui style and colours, Segoe UI / Palatino fonts, the night-sky background |
| `ui/widgets.h/.cpp` | Toggle, help marker, section cards and the setting rows every page uses |
| `ui/shell.cpp` | Sidebar, Play button, page switch and status footer |
| `ui/pages.h`, `ui/page_*.cpp` | One file per page: Play, Video, Audio, Controls, Key Bindings, Mods, DLC |
| `settings_file.h/.cpp` | `kameo_settings.toml` through `core/settings_schema.h`, written on every change |
| `dlc_install.h/.cpp` | Finding LIVE/PIRS/CON packages and unpacking them on a worker thread |
| `level_mods.h/.cpp` | The mods folder, load order and the linked data folder the game runs from |
| `kmod.h/.cpp` | The `.kmod` format: CAFF archives (levels, models) stored as differences |
| `kmod_zlib.cpp` | zlib for `.kmod` via stb (the launcher's only stb_image/stb_image_write implementation) |
| `runtime_loader.cpp` | The delay-load hook that loads `launcher\rexruntime.dll` |
| `builds.h/.cpp` | Builds from the GitHub releases: listing, downloading into `builds\<tag>\`, switching |
| `http.h/.cpp` | HTTPS GETs through WinHTTP |
| `json.h/.cpp` | A small JSON reader for the GitHub API |
| `zip_extract.h/.cpp` | Unpacking a release zip (stored and deflated entries) |

## Settings

Every row reads and writes a cvar by name through `Settings`, using the same
schema as the game (`core/settings_schema.h`), so the names in the page files are
the persisted cvar names and must match the game's `REXCVAR_DEFINE_*` exactly.
Restore Defaults resets the names listed on that page. The display mode falls
back to the main config's fullscreen flag when `kameo_window_mode` is unset.

Key Bindings reproduces the pause menu's rule: binding a key another action
already has gives that action the rebound action's old keys. A capture arms
after 300 ms and times out after 5 s.

## Play

`LaunchGame` starts `kameorepowered.exe` from the launcher's folder. With
mods enabled it first builds `mods_build/assets` — a mirror of `assets/` made of
hard links (skipping `.rex_lang_merged`) with the patched files written over
it — and passes `--game_data_root`. The build is reused while the stamp (asset
path plus each enabled mod's name, size and time) is unchanged. Mods lower in
the list win a file they share, and the Mods page lists such conflicts. An old
build is renamed aside before it is deleted, so while a game still runs from it
(and holds its files open) the rebuild stops with an error instead of emptying
the folder under the game. Making and installing mods: [modding.md](modding.md).

## Builds

The Builds page lists the project's GitHub releases (`MaxDeadBear/KameoRePowered`,
the newest 40) with their notes. A build is downloaded into `builds\<tag>\` and
unpacked there; switching copies its files over the game's own beside the
launcher. The game keeps its settings, config, logs and assets beside its exe,
and released builds can't be pointed elsewhere, so a build is switched in place
rather than run from its own folder.

- Only the build's top-level files are copied, minus `KameoLauncher*`, so the
  launcher and everything the player owns (`assets\`, settings, logs, mods) stay.
- Before the first switch, a game folder build that matches no download is saved
  as `builds\local-<date>\`, listed under Your Previous Builds.
- Each file is copied in beside its target, the old one is moved to
  `builds\.trash\`, then the new one takes its name; any failure moves the old
  files back. `.trash` is emptied when the launcher starts.
- A switch is refused while the game is running: the game's exe can't be opened
  for exclusive access then.
- The build in use is remembered as `active_build` in `kameo_launcher.toml`; with
  none, it is whichever download has the same exe as the game folder.

When `check_for_builds` is on (the default), the list is fetched as the launcher
opens, and the Play page offers the newest release, or pre-release when
`include_prereleases` is on, if it is newer than the build in use. GitHub allows
60 unsigned API requests an hour per address.

## Command line

- `KameoLauncher --install-dlc <folder or file> [--content-root <folder>]`
  installs without a window and reports to the console it was started from; the
  exit code is 2 when nothing was found, otherwise the number of failures.
- `KameoLauncher --list-builds` prints the published builds and which are
  downloaded or in use.
- `KameoLauncher --switch-build <tag>` downloads that build if needed and switches
  the game folder to it; the exit code is 0 when it is in use afterwards.
- `KameoLauncher --build-mods` prepares the mod data folder and prints the
  `--game_data_root` argument the game would get.

DLC is installed under the content root (`user_data_root` from
`kameorepowered.toml`, or `Documents\kameorepowered`) in
`0000000000000000\4D5307D2\00000002`.
