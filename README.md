# Kameo: RePowered

Static recompilation of **Kameo: Elements of Power** (Xbox 360) for Windows
and Linux, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any Kameo code, data, or assets. You provide your own legally dumped `kameo.iso`.

## Using a pre-built release

> Kameo: RePowered is available on [Goopie](https://goopie.xyz)!

Get the latest stable build [here](https://github.com/birabittoh/KameoRePowered/releases/latest).

Nightly builds are available [here](https://nightly.link/birabittoh/KameoRePowered/workflows/ci/main?preview).

Just place the downloaded executable next to the extracted `assets` directory and run it.

## Building from scratch

### 0. Install dependencies

#### Linux (Arch/CachyOS)
```bash
paru -S clang20 cmake ninja vulkan-headers extract-xiso
```

#### Windows
```powershell
scoop install llvm cmake ninja
```

### 1. Clone

```bash
git clone https://github.com/birabittoh/KameoRePowered.git
cd KameoRePowered
```

### 2. Get the ReXGlue SDK

Download the `v0.9.0` release archive for your platform from the
[SDK releases page](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.9.0)
and unpack it anywhere. Everything below refers to that directory as
`$SDK` — it is the one containing `bin/rexglue`, `include/`, and `lib/cmake/`.

> The SDK is published as a **RelWithDebInfo** build and its CMake package only
> exports `*-relwithdebinfo` targets, so the project must be configured with a
> matching preset. Using a `release` preset will fail to link.

### 3. Provide your game

Extract your legally dumped ISO directly into `assets/`:

```bash
extract-xiso -d assets "Kameo - Elements of Power (USA).iso"
```

### 4. Build

Two steps: recompile the guest code, then build the host. The preset is
`win-amd64-relwithdebinfo` on Windows, `linux-amd64-relwithdebinfo` on Linux.

The two variants are fully independent — separate manifests, separate generated
trees, separate build directories — so you can keep both built and switch
between them without re-running the other's codegen.

> **`assets/default.xexp` is the one piece they cannot share.** The loader
> applies it to the base image on every launch, and the recompiled TU code
> assumes the patched layout. So it must be **present** for TU codegen *and*
> TU runs, and **absent** for vanilla codegen *and* vanilla runs — a vanilla
> binary launched with the patch staged will load a patched image its
> recompiled code does not match. Only one variant is runnable at a time out of
> a given `assets/` directory; move the file in and out to switch.

| | Vanilla | Title Update |
|---|---|---|
| Manifest | `kameorepowered_manifest.toml` | `kameorepowered_tu_manifest.toml` |
| Codegen hints | `kameorepowered_config.toml` | `kameorepowered_tu_config.toml` |
| Generated tree | `generated/default/` | `generated/tu/` |
| CMake flag | `-DKAMEO_TU=OFF` | `-DKAMEO_TU=ON` |

#### Vanilla

```bash
# The loader auto-applies assets/default.xexp when it exists, so it must be
# absent here or codegen would silently recompile the patched image instead.
rm -f assets/default.xexp

"$SDK/bin/rexglue" codegen kameorepowered_manifest.toml

cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH="$SDK" -DKAMEO_TU=OFF
cmake --build --preset win-amd64-relwithdebinfo --parallel
```

#### Title Update

Stage the update once, from your own TU package. This picks the variant matching
your base XEX by digest, writes the delta patch to the sibling path the loader
looks for, and extracts the per-language `.str` tables:

```bash
python scripts/extract_tu.py /path/to/TU_* \
  --base assets/default.xex \
  --output assets/default.xexp \
  --update-dir assets/TU
```

Then build against the TU manifest. `assets/default.xexp` must be in place for
this codegen run — that is what bakes the update in:

```bash
"$SDK/bin/rexglue" codegen kameorepowered_tu_manifest.toml

cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH="$SDK" -DKAMEO_TU=ON
cmake --build --preset win-amd64-relwithdebinfo --parallel
```

`-DKAMEO_TU=ON` selects `generated/tu/` and compiles out the hand-written hooks
that call game functions by hardcoded vanilla addresses the update relocates.
Always pass the flag explicitly — it is cached, so reconfiguring without it
reuses the previous value.

A correct TU codegen reports the patch being applied:

```
XEX patch applied successfully: base version: 0.0.0.0, new version: 0.0.2.1
```

#### Building from an IDE

`CMakePresets.json` is deliberately machine-independent, so the SDK path is not
in it. For Visual Studio / VS Code, drop a `CMakeUserPresets.json` next to it
(gitignored) so both variants show up in the preset dropdown:

```json
{
    "version": 6,
    "configurePresets": [
        {
            "name": "kameo-vanilla",
            "inherits": "win-amd64-relwithdebinfo",
            "binaryDir": "${sourceDir}/out/build/vanilla",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "C:/path/to/rexglue-sdk",
                "KAMEO_TU": "OFF"
            }
        },
        {
            "name": "kameo-tu",
            "inherits": "win-amd64-relwithdebinfo",
            "binaryDir": "${sourceDir}/out/build/tu",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "C:/path/to/rexglue-sdk",
                "KAMEO_TU": "ON"
            }
        }
    ],
    "buildPresets": [
        { "name": "kameo-vanilla", "configurePreset": "kameo-vanilla" },
        { "name": "kameo-tu", "configurePreset": "kameo-tu" }
    ]
}
```

Give each variant its own `binaryDir`. Sharing one between an IDE and a terminal
build is what produces `ninja: error: failed recompaction: Permission denied` —
the IDE holds a handle on the build directory, ninja's rename over `.ninja_log`
fails, and the leftover `.ninja_log.recompact` / `.ninja_log.restat` files make
every later build fail the same way until they are deleted.

### 5. DLC (optional)

Place your DLC content packages (STFS files) into the emulator save-data directory and run the extraction script:

```bash
# Linux
mkdir -p ~/.local/share/kameorepowered/0000000000000000/4D5307D2/00000002/
cp /path/to/dlc/* ~/.local/share/kameorepowered/0000000000000000/4D5307D2/00000002/

# Windows
mkdir "%USERPROFILE%\Documents\kameorepowered\0000000000000000\4D5307D2\00000002"
copy C:\path\to\dlc\* "%USERPROFILE%\Documents\kameorepowered\0000000000000000\4D5307D2\00000002"

# Then extract
python scripts/extract_dlc.py
```

This replaces each raw STFS package with a directory of extracted files and generates the `.header` files needed for the game to detect and unlock the DLC.

### 6. Run

Run the executable out of its build directory (the SDK runtime and the
`rexgpu-xenos` plugin are staged there by the build):

```bash
# Vanilla — assets/default.xexp must be absent
./out/build/vanilla/kameorepowered --game_data_root assets --gpu_plugin xenos

# Title Update — assets/default.xexp must be present, and the update partition
# holding the per-language .str tables mounted
./out/build/tu/kameorepowered --game_data_root assets --gpu_plugin xenos \
  --update_data_root assets/TU
```

On Linux with hybrid graphics, prefix with
`__NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only`.

Any flag can be persisted in `kameorepowered.toml` next to the executable
instead of being passed each time — see [Options](#options).

## Options

Options can be persisted by adding them to `kameorepowered.toml` next to the game executable, for example:

```toml
vulkan_device = 1 # NVIDIA GPU
user_language = 4 # French
```

### Keyboard & mouse

Keyboard and mouse controls are enabled by default. Default mapping:

| Input | Xbox 360 button |
|-------|----------------|
| WASD | Left stick |
| Mouse | Right stick (camera) |
| `1` / `2` / `3` | X / Y / B |
| Space | A |
| Left click | LT |
| Right click | RT |
| Q / E | LB / RB |
| Enter | Start |
| Backspace | Back |
| Arrow keys | D-Pad |

All bindings are overridable via `kameorepowered.toml` (or CLI flags). For example:

```toml
keybind_a = "F"
keybind_left_trigger = "LControl"
mnk_sensitivity = 0.5
```

Mouse sensitivity is controlled by `mnk_sensitivity` (default `1.0`).

### Language selection

The game defaults to English. Pass `--user_language <id>` to switch:

| ID | Language   |
|----|------------|
| 1  | English    |
| 2  | Japanese   |
| 3  | German     |
| 4  | French     |
| 5  | Spanish    |
| 6  | Italian    |
| 7  | Korean     |

```bash
./kameorepowered --user_language 6
```

### GPU selection

If you have multiple GPUs, you can force a specific one:

```bash
./kameorepowered --vulkan_device 1
```

List available devices by running the game without the flag.

### Logging

The game writes logs into the `logs` directory by default, but you can configure it.

```bash
./kameorepowered --log_file kameo.log --log_level debug
```

## Adding a hook

1. Find the guest address in `default.xex`.
2. Add to `kameorepowered_config.toml`:

   ```toml
   [functions]
   0x8XXXXXXX = {name = "MyFunction"}
   ```

3. Implement in `src/kameorepowered_hooks.cpp`:

   ```cpp
   void MyFunction(PPCContext& ctx, uint8_t* base) {
       // your logic
   }
   ```

4. Re-run codegen and rebuild.

## Adding a midasm hook (inline patch)

```toml
[[midasm_hook]]
address = 0x8XXXXXXX
name = "MyHook"
registers = ["r3"]
return = true
```

Implement in `src/kameorepowered_hooks.cpp`:

```cpp
void MyHook(PPCRegister& r3) {
    r3.u32 = 1;
}
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [MaxDeadBear](https://github.com/MaxDeadBear)
- [BiRabittoh](https://github.com/birabittoh)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.

The recompiled game code produced at build time contains symbols and logic
from Kameo: Elements of Power and is **not** redistributable. Do not share
`default.xex`, the `generated/` directory, or any built binary that links
against them.
