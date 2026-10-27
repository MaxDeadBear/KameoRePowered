# Stereoscopic 3D

Side by side, top and bottom, and red/cyan anaglyph output. It is off by default (`kameo_gfx_stereo = 0`). When it is off, it costs nothing beyond one zero-valued shader constant.

## How it works

1. The render thread replays each captured frame once per eye. The first eye is copied aside and the second is drawn into a cleared scene target. The draw path's `DrawCapturedScene` takes the eye count for this.
2. Every recompiled vertex shader ends with `oPos.x += g_StereoShift.x * (oPos.w - g_StereoShift.y)` when `oPos.w != 1`. `g_StereoShift` is shared constant c17.yz, and the replay writes it per eye.
3. `StereoShiftsDraw` decides which draws take the shift. These are draws into the scene, and offscreen passes of the scene's 16:9 shape (glow, water). Shadow maps, the imposter atlas and the flat 2D HUD stay unshifted.
4. The focus distance follows the follow camera's distance from Kameo. The game reports it from the camera hook.
5. The composite in `stereo_pass.cpp` lays both eyes onto the swap chain in the chosen format.

## Files that are only 3D

- `src/gpu/stereo/` (this folder)
- `src/gpu/shaders/hlsl/stereo.hlsl` (the composite shader)

## Removing it

Every line outside this folder that touches the feature carries a `STEREO:` tag. This command lists them all:

```bash
grep -rn "STEREO:" src tools README.md
```

1. Delete `src/gpu/stereo/` and `src/gpu/shaders/hlsl/stereo.hlsl`.
2. Delete each tagged line or block:
   - `present.cpp`: the eye count, the `stereo` flag and the `CompositeStereo` branch. Pass `1` for the eyes, or drop the parameter.
   - `scene_replay.cpp`: the shift line in `BuildSharedBlock`. The eye loop collapses to a single pass, with `eye` always 0.
   - `replay_context.h`: the `stereo_shift` field. `g_sceneEye`, `SceneStamp` and `FramesSince` can stay as they are, because with one eye they behave exactly like the old frame counter.
   - `graphics_system.h` and `graphics_system.cpp`: the stereo methods, the members block and the `ReleaseStereoResources()` call in `Shutdown`.
   - `hooks/display.cpp`: the frustum widening.
   - `engine/camera.cpp`: the focus-distance report.
   - `options_table.cpp` and `restore_defaults.cpp`: the four 3D rows and their defaults.
   - `README.md`: the feature line.
   - `tools/shaders/build_builtin_shaders.py`: the two `stereo.hlsl` entries. Then run that script to regenerate `builtin_shaders.h`.
3. Optional: the shader side. The `g_StereoShift` lines are harmless with the constant at zero, so they can be left in place. To remove them:
   - Delete the `KAMEO: stereoscopic 3D` lines from `thirdparty/XenosRecomp`'s `shader_common.h` and `shader_recompiler.cpp`.
   - Regenerate `tools/xenosrecomp/kameo.patch` with `git -C thirdparty/XenosRecomp diff`.
   - Rebuild the recompiler.
   - Run `tools/shaders/refresh_shader_cache.py`, `build_particle_shaders.py` and `build_wet_lens_shader.py`.

   Three vertex shaders crash the recompiler's built-in DXC: 511A3E3E3F0761D3, 04D58836F1C8402F and 519FBEC688E43106. `build_particle_shaders.py` reuses its cached translations when that happens. For the shader cache, compile them with `dxc.exe` using the flags in `dxc_compiler.cpp` and put them in `out/shader_build_cache`.
