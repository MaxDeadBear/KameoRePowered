# Kameo: RePowered

[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/8jaZQUkxZZ)

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

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py
```

This downloads the latest nightly and installs it into `sdk/<platform>/`.

### 3. Provide your game

Extract your legally dumped ISO directly into `assets/`:

```bash
extract-xiso -d assets "Kameo - Elements of Power (USA).iso"
```

### 4. Build

Use this script:

```bash
# Vanilla
python scripts/build.py

# Title Update
python scripts/build.py --tu /path/to/TU_*
```

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

```bash
python scripts/run.py
```

The run script sets up the library path, GPU offload variables, and passes `--game_data_root` and `--update_data_root` (if a TU build extracted update data) automatically.

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
