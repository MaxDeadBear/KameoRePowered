# Native renderer

Work in progress. The goal is to replace the `xenos` GPU-emulation plugin with a
renderer that intercepts the game's own D3D calls, in the style of
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), **without
modifying the ReXGlue SDK**.

## Why this is possible without touching the SDK

| requirement | how it is met |
|---|---|
| replace a recompiled guest function | `REX_HOOK` in `rex/hook.h`. Codegen emits every function as a weak alias (`DEFINE_REX_FUNC`), so a strong definition wins at link time |
| observe without replacing | call `__imp__sub_<addr>`, which `DECLARE_REX_FUNC` also declares. Lets us tee and A/B against xenos |
| supply a custom graphics system | `RuntimeConfig::graphics` takes an injected `IGraphicsSystem`; `gpu_plugin` is only consulted when it is empty |
| skip guest GPU emulation | the ring-buffer entry points on `IGraphicsSystem` are optional and default to no-ops |

The SDK's own graphics stack is *not* reusable: `rexgpu-xenosrd.lib` is a
2 KB import lib exporting only `rex_gpu_create` / `rex_gpu_abi_version`, and
`rexruntimerd.lib` contains no `ShaderTranslator` / `TextureCache` symbols. That
is why shaders are translated offline instead.

## Layout

| file | what it is |
|---|---|
| `kameo_guest_device.h` | reverse-engineered guest `D3DDevice` layout — state tables, register shadow, textures, streams, viewport, constants |
| `SHADERS.md` | the v0x0E shader container format, the XenosRecomp port, and how to build the shader cache |
| `kameo_graphics_system.h/.cpp` | the plume-backed `IGraphicsSystem` skeleton |

## Building

Off by default; the shipping build is unchanged when it is.

```
git clone --recurse-submodules https://github.com/renderbag/plume.git thirdparty/plume
cmake -S . -B out/build/native -G Ninja \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DKAMEO_NATIVE_RENDERER=ON
cmake --build out/build/native --target kameorepowered
```

**After a link fails with "permission denied" (the game was running), DELETE
`kameorepowered.exe` before rebuilding.** lld leaves a ZERO-BYTE exe behind and
the next build reports `[1/1] Linking` and exits 0 without producing anything --
so the run after it silently tests a binary that does not exist. Check the size.

Use clang, not MSVC — the SDK headers need `__builtin_bswap*`, `__VA_OPT__` and
`/utf-8`, so an MSVC configure fails in the SDK before reaching our code.

## Status

Done:

* guest `D3DDevice` layout derived and cross-checked
* shader container format (v0x0E) fully decoded
* XenosRecomp ported; **145/145 shaders** compile to DXIL + SPIR-V via
  `scripts/build_shader_cache.py`
* plume vendored, building, and linked into the target
* `KameoGraphicsSystem` brings up a plume interface, device, command queue and
  swap chain, and logs the adapter it selected

* wired up end to end behind the `kameo_native_renderer` cvar (default off):
  `OnPreSetup` injects the system and clears `gpu_plugin`, and a hook on
  `D3DDevice_Swap` (0x820CF9D8) drives acquire -> clear -> present each frame

## Runtime status: boots to the title menu

**Kameo reaches its title/start menu** with no GPU emulation at all — no
ring-buffer consumer, no command processor, no PM4 parsing, no interrupts. The
guest's own D3D calls are intercepted and frames go out through plume/D3D12.

Nothing is visible yet: the draw calls are stubbed, so the window shows the
clear colour from `PresentClear`. Audio is the way to verify progress, and it is
how every stall in this bring-up was caught.

### What made it run: publish the read-back positions

The guest waits on GPU progress in several places. With nothing consuming the
ring buffer those waits never complete. The insight is that in this design the
GPU genuinely *has* consumed everything — we never read the ring at all — so the
fix is to stop the read-back block lying, not to simulate progress.

There are **three** separate slots, and they must all be published:

| read-back slot | wait function | address |
|---|---|---|
| `readback[0]` | `D3D::CDevice::BlockOnFence` | 0x820CDD80 |
| `readback + 4` | `D3D::CDevice::BlockOnSecondaryPosition` | 0x820CD0C8 |
| `readback + 60` | `D3D::CDevice::BlockOnPrimaryRange` | 0x820CD578 |

`readback` is `*(device + 10384)`. Fixing them one at a time just moved the stall
to the next one, which is why this looked unfixable for several rounds.

Plus the plain waits, which simply return: `BlockUntilIdle`,
`SynchronizeToPresentationInterval`, `BlockUntilNotBusy`.

### Two things that do NOT work — do not retry them

**Do not stub `KickOff` (0x820CDC88).** It recycles the command buffer, and the
game's PM4 writers are *inlined* into its own code, so they cannot be hooked and
the buffer really does fill. Returning the write pointer unchanged makes the
caller's `if (write > limit) write = KickOff(device)` loop spin — measured at
**170 million calls per two seconds**, with no audio and no progress. Let the
original run.

**Do not force `BeginRingAlloc` to allocate from the ring base.** It papers over
the read positions never advancing and changes nothing once they are published.

### A vsync worker is not needed

Dispatching guest vblank interrupts at 60 Hz would be rebuilding Xenia's
GraphicsSystem underneath a renderer meant to replace it. Removing the waits is
smaller and truer to the design, and it is sufficient — the game boots without a
single interrupt being delivered.

### Diagnostics worth keeping

`kameo_gfx_hooks.cpp` carries a host-thread watchdog that survives guest
deadlocks. It logs which hook was entered last and, after two quiet intervals,
suspends every other thread and walks its stack with dbghelp. Because recompiled
guest functions are ordinary x86 symbols named `sub_<guest address>`, the dump
names the exact guest function that is stuck:

```
__imp__sub_820CD0C8   <- BlockOnSecondaryPosition, spinning
__imp__sub_820CD188   <- RingBufferDeviceAllocate
__imp__sub_820CD390   <- BeginRingAlloc
__imp__sub_820C3460   <- D3DDevice_Resolve
__imp__sub_820B7128   <- mainRender
```

That found the stall in one run after two rounds of decompiling the wrong
candidates. Reach for it before theorising.

## First pixels: the Bink videos

Three Binks play at boot — Microsoft, Rare, and the title — and all three are
audibly working. They are the best first rendering target: self-contained, one
textured quad each, verifiable at a glance, and they run in the first seconds so
iteration is fast.

### The path

```
fmvRender2D            0x822648A0
  fmvRenderPlaying     0x82264518   BinkDoFrame decodes
    Sync_Bink_textures 0x822659F8   dcbst cache flush ONLY -- no upload needed,
                                    Bink decodes straight into texture memory
  x_g_kamVideoParams_1 0x82264418   computes letterbox offsets
    Draw_Bink_textures 0x82265558   <-- implement this one
```

### What `Draw_Bink_textures` does

Everything needed is in one function, which is why it is a good target:

| step | detail |
|---|---|
| samplers 0-3 | filter/address state via the device function table |
| textures 0,1,2 | Y, Cr, Cb planes at `a6 + 32*a6[84] + {0,4,8}` |
| texture 3 | alpha plane, optional -- selects the shader variant |
| pixel constants | `SetPixelShaderConstantFN(0, yuvtorgb, 4)` -- the YUV->RGB matrix, 4 float4 at global `yuvtorgb` = 0x8273A5B0 |
| vertex decl | `InitVertexDeclarationFromFVF(0x102)` = position + one texcoord |
| vertex shader | `PositionAndTexCoordPassThrough` (0x82B7198C) |
| pixel shader | `YCrCbToRGBNoPixelAlpha` (0x82B71984), or 0x82B71988 when an alpha plane exists |
| geometry | 4 vertices, stride 0x14 (x,y,z,u,v), `BeginVertices(TRIANGLESTRIP, 4, 0x14)` then an 80-byte memcpy |

### Texture layout — confirmed LINEAR, no untiler needed

`Create_Bink_textures` (0x822653F0) creates every plane as **`D3DFMT_LIN_L8`** —
linear 8-bit luminance. Bink decodes on the CPU straight into that memory, which
is why `Sync_Bink_textures` only has to flush caches. So the planes can be read
and uploaded as-is; none of the 360 texture tiling applies here.

`make_texture` (0x82265280) returns, per plane: the `D3DTexture*`, the **pixel
data pointer**, the **row pitch** (taken from `D3D::LockSurface`, so it is the
real pitch, not simply the width), and the allocation size.

Relative to the struct passed around as `a1 + 4` in the fmv object, with
`idx = struct[84]` the current frame-buffer index:

| plane | texture ptr | data ptr | pitch |
|---|---|---|---|
| Y | `+ 32*idx + 0` | `+ 92 + 48*idx` | `+ 96 + 48*idx` |
| Cr | `+ 32*idx + 4` | `+ 104 + 48*idx` | `+ 108 + 48*idx` |
| Cb | `+ 32*idx + 8` | `+ 116 + 48*idx` | `+ 120 + 48*idx` |
| A | `+ 32*idx + 12` | `+ 128 + 48*idx` | `+ 132 + 48*idx` |

Dimensions: Y and A are `struct[68] x struct[72]`; Cr and Cb are the (halved)
`struct[76] x struct[80]`.

**Caveat before coding**: `Draw_Bink_textures` takes floats and ints mixed, and
Hex-Rays' argument numbering across the PPC float/int register split is not
reliable here — it invented an `a5` the caller never passes. Read the guest
registers in the hook and validate against these offsets rather than trusting
the decompiled signature.

### Current state: video is drawing, three artifacts remain

Implemented and working end to end. The Rare logo and title screen render as
recognisable, broadly correctly-coloured video:

* `src/gfx/shaders/bink.hlsl` -> `bink_shaders.h` via
  `scripts/build_gfx_shaders.py` (DXIL + SPIR-V, committed; the build never
  needs DXC)
* pipeline, descriptor set, sampler, vertex/constant buffers in
  `KameoGraphicsSystem::EnsureBinkResources`
* planes uploaded in `UploadBinkPlanes`, drawn in `DrawBink` from
  `PresentClear`, captured by the `sub_82265558` hook

Verified correct at runtime: `1280x720`, chroma `640x360`, pitches
`1280/768/768` — so the struct offsets, dimensions and pitches above are right.
Colours being broadly correct also confirms the YUV matrix and the Cr/Cb
ordering (`+4` = Cr, `+8` = Cb).

Artifacts seen, all three now traced to one cause (see below):

1. a solid green vertical band, exactly 256 px wide, at screen x=256..511
2. a doubled image — the logo appears once correctly, then again offset right
3. a garbled strip along the top edge

These were data-layout problems, not colour-space ones.

### SOLVED: the plane pointers were one 4KB page too low

All the artifacts had ONE cause. Bink's pixel data starts **4096 bytes after**
the pointer the game stores at `+92/104/116/128`, so every plane was read a
page early.

**The trap, if this ever needs re-deriving:** `4096 mod 1280 == 4096 mod 768 ==
256`, so every piece of *horizontal* evidence points at 256, and a 256-byte
correction really does remove the seam, the green band and the doubling. What it
leaves is a whole number of junk rows — 3 for luma (3*1280) and 5 for chroma
(5*768), which is 3840 bytes in every plane. 3840 + 256 = 4096. That residue was
the flickering line along the top edge; correcting by the page removes it too.

**Why a page — this is D3D's own adjustment, not a fudge factor.** Found later,
while reading `D3DDevice_SetTexture` for the overlay work. It builds the GPU
fetch address as

```c
((((BaseFlush >> 20) + 512) & 0x1000) + BaseFlush) & 0x1FFFF7FF
```

and that first term adds exactly `0x1000` whenever the address is `>= 0xE0000000`
— below that, `(addr >> 20) + 512` never reaches bit 12. Confirmed live: a
texture at `BaseFlush=F3D13002` comes out as fetch address `13D14002`, top nibble
masked away and one page added. The Bink planes sit at `0xED70F000`, in that same
range, so they take the same page.

The hook now applies that formula instead of a constant, so it stays correct for
planes allocated outside the range and matches what the general texture path will
need. Re-verified after the change: scans clean, no seam, leading rows continuous.

That stored pointer is `make_texture`'s `v16`, the raw `memXMemAlloc` base.
`make_texture` then calls `D3D::LockSurface` purely to take its *pitch*, and
throws away the surface pointer LockSurface hands back — so the stored base and
the stored pitch do not describe the same origin. We copied the pitch and the
base from the struct and assumed they matched.

How each artifact falls out of that one skew:

| artifact | cause |
|---|---|
| 256px green band at x=256..511 | chroma pitch 768 vs width 640 leaves 128 padding bytes per row; skewed by 256 they land at columns 128..255 instead of 640..767. Padding is zero, and `cr=cb=0` through the guest matrix is *exactly* pure green |
| image doubled 256px right | luma skews 256 screen px, chroma skews 256 chroma texels = 512 screen px, so the colour ghost trails the luma by 256px |
| flickering line on the top edge | the 3840-byte row residue described above |

Confirmed fixed at runtime: `ScanZeroColumns` reports `none` for guest and
staging on all three planes, the luma seam test finds no outlier in 1280
columns, and the leading rows are continuous with the body of the image.

#### The measurement, so it can be redone

The band colour is not a hint, it is an equation. Reading the real matrix out of
`yuvtorgb` (0x8273A5B0) gives standard BT.601 studio-swing, and the band's exact
RGB `(0, 154, 0)` solves to `Y=16, Cr=0, Cb=0` — video black with both chroma
planes reading zero. In the other capture the band is `(0, 255, 0)` because G
saturates, which is why it looked flat.

Then, on the dumped planes:

* `bink_cr.pgm` and `bink_cb.pgm` have a zero-column run at **exactly [128,255]**
* `bink_y.pgm` has a hard vertical seam at **exactly x=256** — mean
  column-to-column delta 39.7 against a 3.87 background, the only outlier in
  1280 columns
* rolling the luma buffer forward by 256 bytes is the **only** shift that removes
  that seam; 128, 384 and 512 all leave one. The unrolled frame is a coherent,
  seamless Kameo scene

`ScanZeroColumns` in `kameo_graphics_system.cpp` is that check, kept as a
regression test: it scans the guest plane AND the staging buffer, so it says
whether zeros arrive with the data or are introduced by our repack.

#### Why the earlier "the planes are CLEAN" reading was wrong

The PGM dump reads back through the *same* pointer and pitch it is meant to
validate, so a constant byte offset renders as a perfectly plausible image. A
flatness test cannot see a rotation. The dumps were never clean — the zero
columns at [128,255] were sitting in `bink_cr.pgm` the whole time; the check
that produced `flat>=16: []` simply did not look for them. **Any future dump
diagnostic has to test structure (seams, phase) and not just plausibility.**

Also measured: `Draw_Bink_textures` fires EXACTLY ONCE per present, so the
doubling was never multiple draws, and one full-screen quad per frame is right.

#### Ruled out along the way — do not re-investigate

* **`PlacedFootprint` parameters.** Read plume's `toD3D12()`: `rowWidth` becomes
  `RowPitch = blockCount * RenderFormatSize`, so for R8 the values passed are
  correct, and the offsets are genuinely 512-aligned rather than luckily so.
* **A plume map/offset skew.** `D3D12Buffer::map` returns the resource's own
  mapped pointer, the same base the footprint `Offset` is relative to.
* **Descriptor binding and Cr/Cb ordering.** Confirmed against the guest's own
  `Draw_Bink_textures`, which binds Y/Cr/Cb/A to samplers 0-3 in that order.
* **Geometry.** The guest builds its quad with UVs 0..1 and a pass-through
  vertex shader, so the hardcoded full-screen clip-space quad is the right
  shape.

### Bugs already found and fixed here (do not reintroduce)

**Alpha-plane detection read the wrong field.** The hook decided a plane existed
by testing its *data* pointer at `+128 + 48*idx`. `Create_Bink_textures` only
writes those on success and never clears them, so an absent alpha plane leaves
stale garbage there — every video looked like it had alpha, and
`UploadBinkPlanes` read 1280x720 through a junk pointer with a junk pitch. The
guest's own test is the *texture* pointer at `st + 32*idx + 4*i`, which that
function zeroes on every iteration. Gate on that.

**Resizing the window froze the game.** plume's D3D12 `waitForCommandFence` is a
bare `WaitForSingleObjectEx(event, INFINITE)` on an auto-reset event that
`executeCommandLists` signals and the wait consumes — it does NOT check whether
a signal is outstanding. `PresentClear` already waits at the end of every frame,
so the extra wait in the resize path had nothing left to wake it and blocked the
guest thread driving presentation forever. `WaitForGpu()` now tracks whether a
submission is actually in flight. Never call `waitForCommandFence` directly.

**Shared staging region.** The first attempt gave green/magenta output with the
image repeated across the screen. Cause: all four planes shared ONE staging
buffer region. Each plane was
written, a copy recorded, then the same memory overwritten by the next plane --
but `copyTextureRegion` does not execute until the command list is submitted, so
every texture received whichever plane was written last. Each plane now gets its
own offset, with a single map/write/unmap pass before any copies are recorded.

### Implementation plan

Hook `Draw_Bink_textures` (0x82265558) and render it natively, entirely inside
the hook. This does NOT require the general draw path to exist first:

1. read the three or four guest texture pointers and their descriptors
2. upload each plane into a plume texture. Bink decodes on the CPU, so these
   should be LINEAR formats -- confirm before writing an untiler
3. build a quad from the four computed vertices (the offsets arrive as
   `a1`..`a4`, already letterboxed by `x_g_kamVideoParams_1`)
4. YUV->RGB in our own small HLSL pixel shader, fed by the four `yuvtorgb`
   constants read from guest memory at 0x8273A5B0
5. draw it in `PresentClear`'s command list, before the present

This needs the first real plume graphics pipeline (shader, pipeline layout,
descriptor set, vertex buffer), which is exactly the scaffolding the rest of the
renderer will reuse.

`BeginVertices` (0x820CE738) can stay unhooked for this: the hook replaces the
whole function, so the guest never reaches it.

## The title-screen overlay: measured, not guessed

A census hook on every draw entry point (`ReportDrawCensus` in
`kameo_gfx_hooks.cpp`, tees and counts, changes nothing) says what the title
screen actually issues per frame:

```
DrawVerticesUP=148  BeginVertices=150  SetTexture=9
```

Two conclusions, both of which redirect the old plan:

**`drawTextureQuadNoSetup` is never called.** It does not appear in the census
at all. It is a resolve/post blit helper, not the UI path — so "the 2D path
(`drawTextureQuadNoSetup`, UI) for first real pixels" was aiming at the wrong
function.

**`BeginVertices` does not need a scratch buffer for this.** That was listed as
the blocker, but `DrawVerticesUP` (0x820CE9D8) is just

```c
v11 = D3DDevice_BeginVertices(pDevice, PrimitiveType, VertexCount, Stride);
if (v11) dc_memcpy(v11, pVertexStreamZeroData, VertexCount * Stride);
```

— the vertex data arrives as an argument, in guest memory. Hooking
`DrawVerticesUP` gives primitive type, vertex count, a data pointer and a stride
outright, exactly like the Bink hook. `BeginVertices` ≈ `DrawVerticesUP` + 2, so
essentially all overlay geometry comes through it and the scratch buffer is only
needed for the handful of standalone callers.

Only ~9 `SetTexture` per frame against ~148 draws: many small quads sharing a
few textures, which is what font/UI atlas text looks like.

### Vertex layouts actually seen

Logged from the first vertex of each distinct (primitive, stride) pair:

| prim | stride | layout |
|---|---|---|
| 5 (TRIANGLESTRIP) | 24 | `float3 pos` + `D3DCOLOR` (0xFFFFFFFF) + `float2 uv` |
| 5 (TRIANGLESTRIP) | 60 | `float4 pos` with **rhw=1.0** (pre-transformed, x=237.7 y=288.1 in *pixels*) + colour + several texcoords |
| 13 (QUADLIST) | 16 | `float3 pos` + `D3DCOLOR` (0xFF000000) |

`Draw_Bink_textures` uses prim=5 at stride 0x14, and IDA names that
`D3DPT_TRIANGLESTRIP`, which pins the enum: 5 = triangle strip, as on PC D3D9.

The stride-60 shape carries `rhw` and pixel coordinates, so it needs only a
viewport transform — no vertex shader, no constants. That is the cheapest route
to real overlay pixels and is where to start.

### Implemented: the text renders

`overlay.hlsl` + `EnsureOverlayResources` / `EnsureOverlayAtlas` / `DrawOverlay`,
fed by the `DrawVerticesUP` hook. "PRESS START", the copyright lines and
"Developed by Rare." all draw in the right place, batched as ~146 glyphs per
frame in a single draw.

Confirmed by measurement that the stride-60 shape really is the text before any
of it was built: the quads are 12-18px wide and 19-20px tall, advance along one
baseline (x = 237.7, 253.2, 270.9, 284.1, 298.0, **gap**, 319.6, ...) with a word
space, all from one atlas, and then repeat offset by (-1.2, -1.1) for a drop
shadow.

**Use the guest's own transform from vertex shader constants c0..c3** -- do not
hardcode a UI resolution. A fixed 640x480 could never be right everywhere: the
title screen lays UI out in 640x480, but the storybook menu uses 1280x720 (its
coordinates reach x 1183, y 749). The viewport is 1280x720 on both, so dividing
by that is wrong too.

Read at a title-screen text draw, the matrix is exactly a pixel-to-clip
transform for that screen's own resolution:

```
c0 = (0.003125, 0.000000, 0.000000, -1.0)     0.003125 =  2/640
c1 = (0.000000, -0.004167, 0.000000, 1.0)    -0.004167 = -2/480
c2 = (0.000000, 0.000000, 0.010000,  0.0)
c3 = (0.000000, 0.000000, 0.000000,  1.0)
```

Clean -1/+1 translate, unlike the stale reading taken a frame late during the
logo investigation. Applying it row-wise to the float4 position (and dividing by
the transformed w) places text correctly on every screen with no constant to
choose. Constants live at `device + 1920 + 16*reg`.

### The Bink frame must be CONSUMED, not held

`Draw_Bink_textures` submits one frame per present while a video plays, so
holding the last submitted frame meant that the moment playback stopped -- which
is exactly what pressing Start does -- the final frame kept being redrawn
underneath everything, frozen. `DrawBink` now clears `bink_pending_` as it takes
it, so a frame is drawn only when the guest actually produced one and the
background reverts to the clear colour when the video ends.

**The untiler is correct** — verified by dumping the untiled result to
`overlay_atlas.pgm` rather than judging it from the rendered text. It is a clean
CJK font sheet with every glyph crisp. `TiledOffset2D` is the standard
`XGAddress2DTiledOffset` swizzle; the atlas is 2048x1688, format `k_8`, tiled.

### The vertex order is a FAN, not a strip

Measured, and it was the cause of a hole through every glyph:

```
v0=(237.7,288.1) TL   v1=(251.4,288.1) TR
v2=(251.4,307.2) BR   v3=(237.7,307.2) BL
```

That is perimeter order. `Draw_Bink_textures` uses strip order (TL,TR,BL,BR), and
assuming it carried over meant expanding as `{0,1,2, 2,1,3}` -- a correct first
triangle and a bogus second one (BR,TR,BL). Half of every letter sampled the
wrong texels, showing as a diagonal seam. The correct expansion is the fan
`{0,1,2, 0,2,3}`. Cull mode was never involved (plume defaults to NONE).

### The UI space really is 640x480 -- confirmed, not fitted

The fade quads settle it. They are `prim=13 stride=16`, untextured opaque black,
with vertices exactly:

```
(0,0) (640,0) (640,480) (0,480)
```

A full-screen quad in the UI's own space. So 640x480 is the actual coordinate
system, not a fitted constant.

**It is NOT caused by a bad video mode.** A hook on `x_g_kamVideoParams`
(0x820BECA8) that logged before overriding reported the guest UI resolution was
ALREADY 1280x720 -- `XGetVideoMode` returns a HiDef 720p mode and the game takes
its own `kamVideoParams = 1280` branch. Forcing those values changes nothing.
The 2x therefore lives in the UI vertex shader's constants, which also means
these vertices are not truly pre-transformed and `rhw=1.0` does not prove a
pass-through shader.

### The fading logo: characterised, needs the shader transform

| property | value |
|---|---|
| path | **standalone** `BeginVertices`, not `DrawVerticesUP` |
| draw | `prim=5 count=4 stride=24` |
| vertices | `(-256,-256,-10) (256,-256,-10) (256,256,-10) (-256,256,-10)`, uv 0..1 |
| colour | `00FFFFFF` -- **alpha 0**, i.e. mid fade-in |
| texture | 512x512, format **19 = k_DXT2_3** (BC2), tiled |

Two useful consequences: BC2 can be handed to D3D12 without decompressing (only
the block-level untiling is needed), and the quad is a 512x512 square centred on
the origin, matching the texture 1:1.

Reading its vertices needs the deferred trick already used here: the guest writes
them into the ring pointer AFTER `BeginVertices` returns, so dump the previous
call's ring on the next call. No scratch buffer required.

What is missing is the transform. Vertex shader constants live at
**`device + 1920 + 16*register`**, derived from
`D3DDevice_SetVertexShaderConstantFN` and confirmed independently by SHADERS.md,
which puts the vertex constant block at `0x780` = 1920.

**On the TITLE SCREEN the game writes exactly one range, `c0+4`.** That looked
like a large simplification -- one 4x4 matrix per draw, no constant layout to
reverse.

**It does not hold on the menu, so do not build on it.** Re-measuring past
PRESS START found at least 47 distinct ranges reaching **register 236**:

```
c0+4  c4+4  c4+10  c4+12  c12+4  c14+4  c16+4  c17+2  c17+4  c21+2
c23+2  c29+3  c30+200  c31+3  c32+3  c34+3  c35+3  c37+3  c38+3  c232+4
```

`c30+200` is a 200-register block -- a skinning matrix palette. So real geometry
uses the constant file properly and a per-range interpretation is hopeless.

**The useful consequence is the opposite of a simplification, and it is
simpler:** do not interpret ranges at all. XenosRecomp's translated shaders
already assume 256 float4 vertex constants, and the guest device stores exactly
that block at `0x780` (pixel constants at `0x1780`, both 256 registers). So the
general path should upload the **whole constant block verbatim** into a constant
buffer each draw and let the translated shader index it. No reverse-engineering
of individual registers is needed for either stage.

Read at a logo draw, that matrix is

```
c0 = (0.0016, 0.0000,  0.0000, -0.6719)     0.0016 = 2/1280 exactly
c1 = (0.0000, -0.0028, 0.0000,  1.0000)    -0.0028 = -2/720 exactly
c2 = (0.0000, 0.0000, -0.0100,  0.0000)
c3 = (0.0000, 0.0000,  0.0000,  1.0000)
```

so it is a pixel-to-clip matrix, applied row-wise (`clip.x = dot(pos4, c0)`).
Taken at face value it places the 512x512 quad at screen x -52..472,
y -258..258 -- top-left and half off-screen, not where the reference shows the
Kameo logo.

**Unresolved, and worth stating plainly:** that means either the constants are
being sampled at the wrong moment (they are read at the *next* standalone
`BeginVertices`, by which point ~148 text draws have each rewritten c0..c3), or
this 512x512 DXT quad is not the logo at all and the logo is baked into the Bink
video. The second is plausible -- the logo appeared without us ever drawing that
quad. Capture c0..c3 in the same call that draws it before concluding either way.

### Known remaining, for the overlay

1. **The logo does not draw** — see above. It needs the vertex shader constant
   range, then a BC2 upload with block-level untiling.
2. D3DCOLOR is confirmed ARGB: the flashing "PRESS START" reads `05FF5858`
   (alpha 0x05 mid-flash, RGB 255,88,88) and the logo reads `00FFFFFF`
   (alpha 0 mid-fade). Both match what is on screen, so the decode is right.
3. The untextured `prim=13 stride=16` fade quads are not drawn. Harmless for now
   — they are full-screen black fades — but they will matter for transitions.

Batching is per RUN of consecutive glyphs sharing an atlas, not per texture,
because the glyphs alpha-blend and reordering would change the result. The title
screen turns out to use a single atlas, but the structure is there.

### Where the texture descriptor actually lives

This cost two wrong guesses, so it is worth stating plainly.

`D3DDevice_SetTexture` stores the bound `D3DBaseTexture*` at
`device + 12704 + 4*Sampler` (`4 * (Sampler + 3176)`) — the same four slots
`Draw_Bink_textures` releases at `+12704..12716`.

**Do NOT read `D3DBaseTexture::Format` for the GPU descriptor.** The struct is 52
bytes with `Format` at `0x1C`, but reading there returns the resource *name*
string (`"texture04.05.05.0032"`). What `Format[0..2]` holds is only partial
input to the descriptor.

The real Xenos fetch constant is **assembled by SetTexture into the device
shadow at `24 * (Sampler + 48)` = `device + 1152 + 24*Sampler`**, out of
`Identifier`, `BaseFlush`, `MipFlush` and `Format[0..2]`. Read that block: it is
exactly what the console's GPU sampled, and needs no reassembly.

Word 0 is `Identifier`-derived — SetTexture takes `0xFFC003FF` from it and fills
bits 10..21 from sampler state, which pins the fields:

| field | bits |
|---|---|
| type (2 = 2D) | 0..1 |
| clamp modes (from sampler state, not the texture) | 10..21 |
| pitch, in units of 32 texels | 22..30 |
| **tiled** | 31 |

Measured on the title screen, both textures bound during overlay draws:

```
sampler0 TILED=1 pitch=2048 type=2  w0=90024802 w1=13D14002 w2=00D2E7FF ...
sampler0 TILED=1 pitch=32   type=2  w0=80424802 w1=136CB04A w2=0003E01F ...
```

So the untiler is not optional for the overlay. Word 1 is the fetch address, and
it already has the `0xE0000000` page adjustment applied — the same one the Bink
planes needed.

## Past PRESS START: the storybook menu is the general draw path

Start is bound to **Escape** by default (`keybind_start` in the SDK's MnK
driver), or the Start button on a pad. Getting through the title and censusing
the menu changes the picture completely:

| | title screen | storybook menu |
|---|---|---|
| `DrawIndexedVertices` | 0 | **~4,100 / frame** |
| `SetTexture` | 9 / frame | **~6,040 / frame** |
| `BeginVertices` | 150 / frame | 547 / frame |
| `DrawVertices` | 0 | 36 / frame |
| `drawTextureQuadNoSetup` | 0 | 3 / frame |

The menu is a full 3D scene, not an overlay. It also introduces a new shape,
`prim=13 stride=36` (9 floats), whose first vertex is `(85.8, -10.8, -25.4)` --
model-space geometry, not UI. So the 2D fast path that carries the title screen
does not extend here: this needs the general draw path (resource tracking for
vertex/index buffers, the shader cache from `SHADERS.md`, a texture cache with
untiling across formats, and render state from the register shadow).

**Correction, and it matters:** the UI coordinate space is NOT fixed. The title
screen is 640x480 (proven by the fade quads), but on the menu the same
stride-60 text path reaches **x 40..1183, y 33..749** -- roughly 1280x720. So
the hardcoded `kUiVirtualWidth/Height` is title-specific and will place menu
text at half scale. That is more evidence the transform belongs in the vertex
shader constants rather than a constant here, and it should be replaced as soon
as the shader path can supply it.

### Bindless draw path: foundation built

`EnsureDrawPathResources` creates the layout the translated shaders require and
it succeeds on the real device:

```
bindless draw path ready (4096 texture slots, 256 samplers)
```

* pipeline layout with **one descriptor set per HLSL space** -- set index ==
  space, so the order is load-bearing: 0/1/2 textures (2D/3D/cube), 3 samplers,
  4 the three constant buffers
* unbounded arrays via `RenderDescriptorSetDesc::lastRangeIsBoundless` with a
  `boundlessRangeSize` upper bound
* constant buffers sized 256 float4 per stage, matching the guest blocks at
  `0x780` / `0x1780`

Two plume API notes that cost a build each: `descriptorIndexing` lives on
**`device_->getCapabilities()`**, not the interface's, and there is no
`RenderBufferDesc::ConstantBuffer` -- use `UploadBuffer(size,
RenderBufferFlag::CONSTANT)`.

Still to do on this path: register guest textures into the heap and **patch
their descriptor indices into the constant data** (the shaders look textures up
by index, so binding alone does nothing), build input layouts from the
declaration, create pipelines per shader-pair + declaration + state, and issue
the draws.

### Shader path: built and wired

Done:

* `src/gfx/shader_cache.cpp` generated and compiled into the target, with a
  hand-written `src/gfx/shader_cache.h` (XenosRecomp emits the .cpp only)
* xxHash pulled in **header-only** (`XXH_INLINE_ALL`) -- this project never
  enables the C language, so adding `xxhash.c` as a source fails the CMake
  generate step with a missing `CMAKE_C_COMPILE_OBJECT`
* hooks on `D3DDevice_CreateVertexShader` (0x820C86D0) and
  `CreatePixelShader` (0x820C8360) resolve each guest shader object to a cache
  entry at creation, and capture unresolved containers for offline translation
* the runtime hash is confirmed byte-identical to XenosRecomp's cache key

* **the blobs load into the real device**: `EnsureShaderModules` creates plume
  shaders from the cache, reporting **972 created, 0 failed (dxil)** on the way
  to the title screen. Entry point is `"main"` for both stages

Modules are built on the presenting thread rather than in the hook -- the
creation hooks run on guest threads and only queue, matching how the Bink and
overlay resources are handled.

Validating this early was deliberate: a wrong entry point or a malformed blob
would otherwise have surfaced much later as an opaque pipeline creation failure,
with the draw path already built on top of it.

Still needed to actually draw with them: build input layouts from the vertex
declaration (the reason UnleashedRecomp translates the *authored* shader rather
than reproducing runtime fetch patching), bind the constant blocks, and track
vertex/index buffers.

## Guest device state map

Everything the draw path needs, resolved from the setters rather than inferred.
All offsets are from the `D3DDevice*` (r3 on every one of these calls).

| state | offset | notes |
|---|---|---|
| vertex shader constants | `1920` (`0x780`) | 256 float4; upload wholesale |
| pixel shader constants | `6016` (`0x1780`) | 256 float4 |
| texture fetch constant | `1152 + 24*sampler` | assembled Xenos descriptor, 6 dwords |
| vertex declaration | `11408` | `SetVertexDeclaration` |
| index buffer | `12532` | `SetIndices` |
| stream N byte offset | `12556 + 8*N` | `SetStreamSource` |
| stream N buffer | `12560 + 8*N` | `D3DVertexBuffer*` |
| stream N stride | `12688 + N` | **u8, in DWORDS** -- multiply by 4 |
| bound texture | `12704 + 4*sampler` | `SetTexture` |
| viewport x/y/w/h | `12808` / `12812` / `12816` / `12820` | `SetViewport` |
| current vertex shader | `20440` | |

**Data addresses are NOT uniform across resource types.** This has now caught us
three times, so take it from the dump rather than assuming:

| resource | where its data address lives |
|---|---|
| texture | `BaseFlush` (`+20`), masked `0xFFFFF000` + page bias |
| **vertex / index buffer** | **a Xenos vertex fetch constant at `+12` / `+16`** |
| shader | neither -- patched ucode pointer at `+40`, see SHADERS.md |

For textures, mask `BaseFlush` with `0xFFFFF000` and add the page bias
`(((addr >> 20) + 512) & 0x1000)` -- the same adjustment D3D itself applies, and
the one the Bink planes needed.

For buffers, `BaseFlush` reads **3**, not an address. A dumped vertex buffer:

```
00:00090001  04:00000000  08:FFFF0000  12:F40912C3  16:00000062  20:00000003
```

`+12` is `type:2 | address:30` (type 3 = vertex fetch, so the address is
`value & ~3`), and `+16` is `endian:2 | size:24` with the size in **dwords**.
The address sits in the same physical range as texture addresses.

### Vertex declaration layout (for building input layouts)

From `D3DDevice_CreateVertexDeclaration`: it allocates `12*count + 40`, writes
the element count and max stream index into the header, then memcpys the
caller's element array in.

| field | offset |
|---|---|
| element count | `+8` |
| max stream index | `+12` |
| element array | `+36`, **12 bytes per element** |

Per element:

| field | offset | type |
|---|---|---|
| Stream | `+0` | u16 (255 = `D3DDECL_END`) |
| Offset | `+2` | u16 |
| Type | `+4` | u32 XGVERTEXFORMAT |
| Method | `+8` | u8 |
| Usage | `+9` | u8 (D3DDECLUSAGE) |
| UsageIndex | `+10` | u8 |

**Xbox's `_D3DVERTEXELEMENT9` is 12 bytes, not the 8 PC D3D9 uses** -- `Type` is
a 32-bit XGVERTEXFORMAT, not a WORD. Assuming the PC layout would walk the array
at the wrong stride and produce plausible-looking garbage.

Verified against a live declaration, with offsets advancing exactly by each
element's size:

```
elements=8 maxStream=0
[0] offset=0  type=002C2359 usage=5   TEXCOORD0     4 bytes
[1] offset=4  type=002C2359 usage=5   TEXCOORD1     4 bytes
[2] offset=8  type=001A235A usage=0   POSITION      8 bytes
[3] offset=16 type=001A235A usage=2   BLENDINDICES  8 bytes
[4] offset=24 type=002A2187 usage=3   NORMAL        4 bytes
[5] offset=28 type=002A2187 usage=7   BINORMAL      4 bytes
[6] offset=32 type=002A2187 usage=6   TANGENT       4 bytes
```

Note the components are **packed, not float3/float4** -- position is 8 bytes and
normals are 4.

#### XGVERTEXFORMAT decoding -- solved, and already written

Do not reverse the bitfield. These are the same `XGVERTEXFORMAT` constants
UnleashedRecomp already enumerates (`GuestDeclType` in its `gpu/video.h`) and
maps in `ConvertDeclType`. Every value Kameo emits matches, and every size agrees
with the element offsets measured above:

| value | type | plume format | bytes | seen as |
|---|---|---|---|---|
| `0x2C2359` | `SHORT2` | `R16G16_SINT` | 4 | TEXCOORD |
| `0x1A235A` | `SHORT4` | `R16G16B16A16_SINT` | 8 | POSITION, BLENDINDICES |
| `0x2A2187` | `DEC3N` | `R32_UINT` | 4 | NORMAL, TANGENT, BINORMAL |
| `0x1A2086` | `UBYTE4N` | `R8G8B8A8_UNORM` | 4 | COLOR |

`DEC3N` is a packed 10:10:10 normal unpacked in the shader, which is why it maps
to `R32_UINT` rather than a vector format. Port `ConvertDeclType` wholesale
rather than deriving a new mapping; the remaining entries (FLOAT1-4, D3DCOLOR,
UBYTE4, SHORT2N/4N, USHORT2N/4N, UINT1, DEC3N variants, FLOAT16_2/4) will show up
as more of the game is reached.

### Geometry reads correctly -- verified on live draws

```
vb addr=F2DF3AA0 sizeDwords=490 stride=40 posOff=8 posType=SHORT4
  v0 pos shorts = 24572 -3041 0 1
  v2 pos shorts = -24629  1621 0 1
ib addr=F2DF4260 first indices = 48 42 8 41 8 12
```

Buffers resolve through the fetch constants, indices are small and in range, and
stride/offsets agree with the declaration. The vertex plumbing works.

### There is NO shortcut around the translated shaders

A debug pass that draws this geometry with a simple shader of our own looks
attractive, but it does not work, and the reason is worth recording so it is not
attempted twice.

Positions are `SHORT4` packed integers with a constant `z=0, w=1`. The translated
vertex shader takes them as **`float4 iPosition0 : POSITION0`** -- the integers
convert straight to float and `w=1` is the homogeneous coordinate. The
fixed-point scale is folded into the transform matrix in the shader's own
constants, so there is no separate scale factor to recover.

That means the raw positions are not interpretable without running the shader,
and the shader cannot run without the bindless binding model above. Geometry and
shaders cannot be brought up independently -- the bindless infrastructure is on
the critical path for anything beyond the 2D overlay.

The same signature also shows what these draws are: 16 texcoords, a tangent
frame, and `BLENDINDICES`/`BLENDWEIGHT`. Real skinned game geometry, consistent
with the `c30+200` bone palette found earlier.

### Why `DrawIndexedVertices` takes no buffers

It emits PM4 directly and reads everything from the state above -- the index
buffer address comes out of `pDevice[1].m_Constants.Fetch[15]`. There is no
argument to intercept, which is why the draw path has to reconstruct state from
the device rather than from call arguments, unlike `DrawVerticesUP`.

Vertex data reaches the GPU through **vertex fetch constants**, not fixed-function
streams, which is exactly why UnleashedRecomp translates the authored shader and
feeds native input layouts built from the vertex declaration: it avoids
reproducing runtime fetch patching entirely. Follow the same route here.

### Then, in order

1. resource creation hooks: `CreateTexture`, `CreateVertexBuffer`,
   `CreateSurface`, `CreateVertexDeclaration` — back each with a plume resource
2. `BeginVertices` (0x820CE738) scratch buffer, for the standalone callers
3. draw submission for `DrawIndexedVertices` / `DrawVertices`. Read state from
   the register shadow at `device + 0x480` and issue native draws, using the
   shader cache from `SHADERS.md`
4. replay each shader's definition table into the register shadow at bind time
   (offline baking is disabled — see `SHADERS.md`)
5. `Resolve` and `Clear`/`ClearF` — currently no-ops
6. collapse the predicated EDRAM tiling to a single full-resolution pass

## How the hooks stay safe

`kameo_gfx_hooks.cpp` defines a strong `sub_820CF9D8`, which overrides the weak
alias codegen emits. When the renderer is not active it calls
`__imp__sub_820CF9D8` — the original body — so a build with the renderer
compiled in but the cvar off behaves like a stock build. That same tee pattern
is what will let draw calls migrate one family at a time while diffing against
xenos.

## Runtime obligation carried over from the shader work

Offline constant baking is disabled (`kBakeDefinitionTable` in the vendored
recompiler). The renderer must therefore **replay a shader's definition table
into the guest register shadow when binding it**, which is what
`D3DDevice_SetPixelShader` does natively — `{u16 dstByteOffset, u16 dwordCount}`
records copied to `device + 0x480`. See `SHADERS.md`.

## The general draw path: implemented, and the scene renders

`src/gfx/kameo_draw_path.cpp` implements what the sections above were building
towards. The storybook menu and in-game levels render: terrain, foliage,
skinned characters, lighting, the book with its wooden covers and banner, the
A/B glyphs, text, and the water refraction.

Structure — capture on the guest thread, submit on the presenting thread:

| stage | what it does |
|---|---|
| capture | copies EVERY byte a draw depends on (vertices, indices, both constant blocks, the 16 fetch constants, render state) into a per-frame arena, because the guest overwrites all of it later in the same frame |
| submission | owns the plume caches (textures, samplers, pipelines, offscreen targets) and replays the frame as one ordered command stream |

A captured frame is an ordered stream of commands, not a bag of draws:
`Draw`, `Resolve` and `Bink` are interleaved exactly as the guest issued them.
That ordering is load-bearing — see the fade quad below.

### Traps that cost real time here

**plume ignores the HRESULT from `CreateGraphicsPipelineState`.** A rejected
PSO still returns a non-null wrapper whose `d3d` is null, and the next
`setPipeline` dies inside `d3d12.dll` with nothing logged. A null check on the
returned pointer does NOT catch it; `PipelineCreated()` reaches through
`plume_d3d12.h` and checks the backend object. Same class of trap as
`waitForCommandFence` above.

**The shader cache was `lib_6_3` libraries.** XenosRecomp compiles as a shader
LIBRARY whenever `specConstantsMask != 0`, so the host can supply the constant
at state-object link time. A library cannot go into a graphics PSO — D3D12
rejects it with `Shader version provided: UNRECOGNIZED` and `Input Signature in
bytecode could not be parsed`. Both spec constants are fixable at compile time
for this title, so `shader_common.h` now DEFINES `g_SpecConstants()` and
everything builds as plain `vs_6_0` / `ps_6_0`. ALPHA_TEST being always-on is
exact provided the renderer writes a threshold of 0 when the guest has alpha
testing off — `clip()` only discards on a negative argument.

**Primitive 5 is a triangle FAN, not a strip.** These are the XENOS values, not
PC D3D9's. IDA's `D3DPT_TRIANGLESTRIP` label comes from a PC header and is
wrong here. Two independent measurements agree: the UI emits quads in perimeter
order, which only closes as a fan, and 13 = QUADLIST exists at all.

**Do not add the "D3D page bias" on top of `REX_PHYS_HOST_OFFSET`.** The SDK
maps guest physical memory (>= 0xE0000000) one page up on Windows, and for every
address these resources use that is numerically identical to D3D's own
`(((addr >> 20) + 512) & 0x1000)`. Only ONE may be applied. This file always
uses raw guest addresses through the SDK macros. (The older Bink/overlay code
adds it by hand and reaches the same pointer, which is why both work.)

### Measured, not guessed

**Render state** comes from Kameo's own setters, reached through the device's
function table, so the offsets and bit positions are authoritative for this
build — see the provenance block in `kameo_gfx_state.h`. Note
`SetRenderState_AlphaBlendEnable(FALSE)` writes ONE/ZERO/ADD into
RB_BLENDCONTROL0 itself and the factor setters return early while blending is
off, so that one register describes the effective blend with no separate enable.

**Index width** is bit 31 of the index buffer's `Common` word:
`DrawIndexedVertices` tests exactly that, taking `StartIndex*4` when set and
`StartIndex*2` when not.

**Vertex declaration** is at `0x2C90`, which is what `D3D::ProcessLazyStreams`
reads. `kVertexDeclaration = 0x3010` in `kameo_guest_device.h` is the separate
FVF slot and nothing in the D3D range reads it. Pixel shader is at `0x3290`,
vertex shader at `0x4FD8`.

**XGVERTEXFORMAT decodes as a bitfield** rather than needing an enum table:
bits 0..5 are the Xenos vertex format, bits 8..11 the interpretation (0 UNORM,
1 SNORM, 2 UINT, 3 SINT/FLOAT, 8 D3DCOLOR). Checked against every value Kameo
emits.

### Four bugs that all looked like "the shader is wrong"

Each rendered as flat black or missing geometry, and none was a shader problem:

1. **DEC3N normals decoded as 11:11:10.** XenosRecomp picks `tfetchR11G11B10`
   from the USAGE (Normal/Tangent/Binormal), not the declared format, because
   Sonic Unleashed's normals really are 11:11:10. Kameo's are DEC3N (2:10:10:10
   signed). Wrong field widths gave garbage tangent frames and N·L clamping to
   zero over whole meshes. The capture now repacks DEC3N into the biased layout
   the shader reconstructs, exactly; the shader is untouched.

2. **The fetch constant's SWIZZLE was ignored.** A `k_8` font atlas is one
   stored channel that the swizzle fans out to the components the shader
   multiplies by. Mapping it to `R8_UNORM` gives `.gba` = 0,0,1, so all text
   drew black. Byte-channel formats with a non-identity swizzle are now expanded
   to RGBA8 with the swizzle baked in (plume has no per-view component mapping).

3. **Empty resolve targets.** The guest renders a pass, resolves it into a
   texture, then samples that texture. With no resolve implemented those
   destinations held whatever guest memory held — zeros — and every surface
   sampling one drew flat black. An all-zero check on uploaded textures is kept
   as a diagnostic: it separates "texture missing" from "texture present but
   never rendered into", which need completely different fixes.

4. **The fade quad, drawn out of order.** An untextured full-screen black quad
   the guest paints over. Drawing Bink before the scene unconditionally put the
   video underneath it and the frame went black. Bink is now a command in the
   stream.

### The storybook pages are render targets

The menu renders each page's content into its own target (640x480, 240x320,
480x640), resolves it to a texture and maps that onto the curved page mesh.
Rendering those passes into the swap chain put the page content on the SCREEN
and, via the resolve, on the page as well — the text appeared twice, once flat
and once wrapped. A draw is redirected to an offscreen target when its viewport
is not the guest's full 1280x720 frame, which distinguishes an offscreen pass
from the main scene without reversing the D3DSurface layout.

Resolve targets are keyed on guest address AND dimensions. Address alone
aliases: the guest reuses that memory for ordinary textures, and handing those a
copy of the framebuffer replaced the parchment pages with the purple background
behind them.

### A resolve must name its own source surface

The storybook pages came back purple — the book covers, banner and plain text
rendered correctly while the pages showed the background behind them, and the
KAMEO headings, Kameo's portrait, the button plates and the A/B glyphs all
vanished with them. Everything lost was composited through a page render
target; everything that survived was drawn straight to the frame.

`CaptureResolve` never recorded WHICH surface the resolve reads from, so the
submission loop inferred it from whatever the previous DRAW had set. That is
only right when the page pass immediately precedes its own resolve. When those
draws went elsewhere in the stream — or never reached submission at all — the
copy silently took the swap chain, and the frame buffer got blitted over the
page texture.

The measurement that settled it: run 099 created exactly ONE offscreen target
(480x640) while THREE page resolve targets (640x480, 240x320, 480x640) were
registered and sampled. Two pages therefore had no offscreen pass at all, and
their resolves could only have copied the swap chain. Counting created targets
against registered resolve targets is the diagnostic — they must match.

Why the viewport heuristic missed them: it only recognises a page pass when the
guest ALSO shrank the viewport, and two of the three pages keep the full
1280x720 viewport. The surface is the authoritative signal, not the viewport.
A resolve now records `rt_surface` (the device's render target at the time of
the call, `dev::kRenderTarget0`), a draw is offscreen when its surface is not
the scene's, and an offscreen surface's dimensions come from the resolve that
consumes it via a one-time scan of the captured frame.

An offscreen resolve with nothing rendered into it this frame is now SKIPPED
rather than copying the swap chain. Skipping leaves the last good contents in
place; copying is always wrong. It logs once per address.

This also explains an artefact that had been read as cosmetic: the menu text
appearing twice, once flat at screen scale and once wrapped on the page. The
page pass was rendering into the swap chain, so the flat copy WAS the page
content. Once the passes render into their own targets it is gone.

### The legacy 2D overlay path is retired

`kameo_gfx_legacy_overlay` now defaults FALSE. The overlay path cannot render
into a page target, so with it on the menu's text was drawn to the screen in
640x480 UI space and scaled up: oversized page text, empty button plates, and
no A/B glyphs on the banner. Through the general path all three page passes
render into their own targets and the menu matches the console. The old path is
still reachable by setting the cvar true.

### A resolve target that changes size removes the DEVICE

Symptom: load a level, return to the main menu, and the picture freezes while
the process stays alive and keeps presenting.

`PerformResolve` cached each destination by guest ADDRESS alone and kept the
size it was first created with. The guest reuses one address for targets of
different sizes -- FDABE000 is registered 256x256 and later resolved 640x480 --
so we issued a copyTextureRegion of a 640x480 region into a 256x256 texture.
D3D12 answers an out-of-bounds copy with DXGI_ERROR_INVALID_CALL and REMOVES
THE DEVICE. After that every buffer map fails, every draw is dropped, and the
renderer presents empty frames forever.

This is the same address-aliasing family as the purple pages: `EnsureTexture`
was taught to check dimensions before SAMPLING a resolve target, but
`PerformResolve` was never taught to check them before WRITING one.

The chain took three measurements, each of which killed a wrong theory:

1. `<ring> ring FAILED TO MAP` -- `write()` returned kNoData for "ring full"
   and "not mapped" alike, so a frame with the rings unmapped looked like ring
   exhaustion. It reported 0 KB used and 0 exhausted, which is impossible for a
   genuinely full ring. That is what pointed at mapping rather than capacity.
2. `GetDeviceRemovedReason` -- separates "the GPU died" from "we have a bug".
   Both present as every draw silently vanishing.
3. The reason code itself: 0x887A0001 is INVALID_CALL, not a hang. A hang would
   have meant a bad draw; INVALID_CALL means a bad PARAMETER, which is what
   turned attention to the copy extents.

Fixed by rebuilding the target at the new size (reusing its heap slot so the
descriptor stays valid) and clamping the copy box against both source and
destination WITH the offset included -- the old clamp bounded `copy_w` by the
source width and then added `left`, so a non-zero source rect could run off the
source too.

Keep `--kameo_gfx_debug_layer=true` in the loop when touching copies. A clean
run with it on is real evidence; without it an invalid call is silent until the
device disappears.

### Vertex reuse must verify CONTENTS, not just the pointer

The per-frame vertex reuse cache keyed on the stream pointer, window and
layout, on the assumption that two draws sharing those share every vertex byte.
That holds for the same mesh drawn twice (shadow pass, then main pass) but NOT
while assets stream: the guest loads a different mesh into the same address
later in the same frame, and the second draw then renders the first mesh's
vertices through its own indices. Visible as geometry exploding across the
screen as things pop in and out.

Same hazard the DrawVerticesUP exclusion already documents; ordinary buffers
simply never got the same protection. The cache now stores an XXH3 of the
source bytes and only honours a hit when the source still hashes the same.
Hashing is a sequential read of bytes the transcode would read anyway -- no
write, no per-element conversion -- so it is much cheaper than the copy it still
avoids in the common case.

Measured, not assumed: the scene line reports `N restreamed vertex buffers`,
and a level run shows 100-480 per 120-frame window. The bounds side was NOT the
problem -- `whole` is already clamped to the smallest stream's capacity from the
fetch constant.

### `shader_dump/` is relative to the WORKING DIRECTORY

The creation hook writes unresolved containers to the relative path
`shader_dump/<hash>.bin`, so they land next to wherever the process was
launched FROM, not next to the exe. Launching from the repo root puts them in
`<repo>/shader_dump` while the corpus the cache was built from sits in
`out/build/native/shader_dump`, and it looks exactly like nothing is being
captured -- 807 cache misses with the old directory's file count unchanged.
Check both, and merge before rebuilding:

```
cp -n shader_dump/*.bin out/build/native/shader_dump/
```

The filename IS the hash, so merging deduplicates itself.

### Looking at the textures: `--kameo_gfx_dump_textures`

Writes every texture to `texdump/<addr>_WxH_fmtN.tga` and logs one line per
texture with its xenos format, swizzle, and what is actually in the texels:
what percentage of the alpha is zero, what percentage is opaque, and what
percentage of the RGB is black.

**It decodes BC1/BC2/BC3 for the dump only.** The upload path hands compressed
blocks straight to the GPU, which is right -- but it also meant the dump saw
only the uncompressed minority. The format census says DXT1 alone is ~90% of
this game's textures (measured: fmt 18 x893172 lookups against fmt 6 x27887),
so without decoding, the dump showed a small and completely unrepresentative
slice, and reasoning from it was worthless.

What it settled: a surface drawing black is one of three different faults, and
the magenta fallback and all-zero check together cannot separate them.

| dump says | means |
|---|---|
| not in the dump at all | the texture is never created -- look at capture |
| real pixels, sensible alpha | the texture is fine, we are DRAWING it wrong |
| 100% black AND 100% transparent | a render target nothing ever rendered into |

Measured in a level: of 249 textures, exactly FOUR are genuinely empty --
FDA80000 640x480, FDBAC000 240x320, FDB45000 640x480, FDC71000 240x320. All
four are render targets. Everything else has content. So the black inventory
slots and black book pages are NOT a transparency or texture-format problem;
they are passes that never render.

Note a swizzle can legitimately force alpha: the foliage normal map at F7598000
has swizzle 000A0A, whose w component is 5 = constant ONE. "alpha 255 in 100%"
there is the guest asking for opaque, not us losing the channel. Decode the
swizzle before calling an alpha wrong.

### Do NOT use "zero draws targeted this surface" to spot EDRAM resolves

Kameo builds its bloom chain by resolving the scene out of EDRAM with no
geometry (512x512, 426x240, 160x90, 32x32), so those resolves genuinely have no
pass. Tempting discriminator -- and wrong: the storybook page targets have zero
draws by the same measure, so the rule copied the purple background over the
finished pages and reintroduced the exact bug the skip was added to fix. Tried
and reverted; the branch is left in place disabled with this note. A correct
discriminator still needs finding, and the bloom textures stay black until then.

### RenderDoc: what it can and cannot tell you here

RenderDoc 1.45 installs via `winget install BaldurKarlsson.RenderDoc`. Launch
the game under it with:

```
renderdoccmd capture --working-dir <repo> --capture-file <repo>/rdcaps/kameo     --opt-ref-all-resources out/build/native/kameorepowered.exe     --kameo_native_renderer=true
```

`--opt-ref-all-resources` matters: it keeps every live resource in the capture,
not just what the frame touched, so a render target that is NEVER drawn into
still appears -- and its absence is usually the thing being investigated.

**The renderdoc-mcp servers cannot work with a stock RenderDoc.** They need
`import renderdoc` from Python 3.10+, and RenderDoc ships NO `renderdoc.pyd` in
either the MSI or the zip, and embeds Python 3.6. Building the module against a
modern Python means building RenderDoc from source. Drive it instead with:

```
qrenderdoc.exe --python script.py
```

which runs inside RenderDoc's own 3.6 where `renderdoc` IS importable
(`ReplayController` and `OpenCaptureFile` both present). It opens the GUI after
the script, so have the script write results to a file and end with `os._exit(0)`.

**Bindless defeats per-draw resource inspection.** Every texture lives in one
descriptor heap that the shaders index into, so `GetReadOnlyResources` returns
nothing per draw and RenderDoc cannot say which texture a given draw samples.
Render targets, depth targets and copies ARE all visible, which is what makes it
useful. Do not expect "which draw reads this texture" to be answerable this way.

What one capture settled that in-engine counters had not, over several rounds:
the 768x768 and 1024x1024 shadow maps receive 94 draws EACH -- the depth-only
passes render correctly -- and are then never copied out. The only depth copy in
the frame is the 1280x720 scene depth. So the shadow chain breaks at the copy,
not at the rendering, which is a much smaller target than "shadows do not work".

### Black grass and black imposter icons: NOT a shader translation bug

Ruled out properly rather than by assertion. The guest shader hash is logged per
draw, which IS the container filename in `shader_dump/`, so the exact shader can
be pulled and run through XenosRecomp by hand:

```
XenosRecomp.exe <hash>.bin out/<hash> thirdparty/XenosRecomp/XenosRecomp/shader_common.h
```

Both shaders behind the black surfaces translate CORRECTLY:

```hlsl
oC0.xyzw = max(r0.xyzw, r0.xyzw);              // r0 = iColor0 -- pure passthrough
r1.xyz = (tex0*r3 + tex1 - c0).xyz * r4.xxx + c0.xyz;   // r4 = iColor0
```

Both multiply by **iColor0, the vertex colour**. Correct code still outputs
black when that input is zero, which is why "all shaders compile" was never
evidence that shading was right -- a distinction worth keeping in mind.

The lead: the game has a CONSTANT COLOUR override. `imposterModelRender` writes
255 into the four globals at 0x82B718A8..B4 and sets `isConstantColourEnabled`
(0x82B718B8) to 1 immediately before drawing. That flag is read in 30+ places --
imposterModelRender, imposterBillboardRender, gameobjRender, simpleobjRender,
x_g_modelTable. If the colour it supplies never reaches the shader on our side,
everything using it draws black, while geometry carrying real per-vertex colours
(trees, terrain) is unaffected. That matches the observed split exactly.

FOUR hypotheses have been tested and KILLED. Do not re-run these:

| hypothesis | how it died |
|---|---|
| grass draws are dropped | 1680 QUADLIST captured / 1680 drawn -- none dropped |
| a null pixel shader made us treat grass as a shadow pass | grass_coreRender DOES bind a PS before drawing; grass_initRender only nulls the slot during setup |
| the shaders translate wrong | pulled both by hash, ran XenosRecomp: translations are CORRECT (see the HLSL above) |
| the COLOR attribute is missing from the declaration | logged it: `COLOR-in-decl=YES` for every grass-family draw. The one NO is a position-only, colormask-8 alpha pass, where that is correct |

So: the geometry is drawn, the shader is right, and the colour attribute is
declared and fed. What is left is the VALUES.

**The next measurement** (narrow, one log line, no new theory required): dump the
transcoded vertex bytes for one grass draw and read the colour.

* colour bytes ZERO -> our D3DCOLOR transcode is at fault. It maps kSwap32 +
  B8G8R8A8_UNORM, which looks right on paper (guest big-endian ARGB swaps to
  B,G,R,A in memory order, matching DXGI's B8G8R8A8 byte order) -- so verify
  rather than re-reason about it.
* colour bytes WHITE -> the fault is the pixel-shader constant `c0` that the
  shader blends against, i.e. the constant upload, not the vertices.

The best grass candidate is ps-hash E5F318C38861EDB5 (samples two textures,
multiplies by iColor0, stride 36, usages POSITION/COLOR0/COLOR1/TEXCOORD0/1).
The constant-colour lead below is still open but is NOT yet tied to grass.

Old next step, still unresolved: find where the constant-colour globals are
written to a shader constant register and check whether the capture uploads it.

Also measured here: QUADLIST draws are ALL submitted (1680 captured / 1680
drawn), so nothing is being dropped -- the geometry reaches the screen and comes
out black. And RenderDoc showed the imposter passes running and leaving their
render target at [0,0,0,0], so the icons are not a missing pass either.

### Black grass: the colour was never the problem

The section above chased the vertex COLOUR, and it was looking in the wrong
place. Measured, on a live grass draw (`ps-hash E5F318C38861EDB5`, stride 36):

| input | measured | verdict |
|---|---|---|
| vertex COLOR0 | `896D5276` | NOT zero |
| vertex COLOR1 | `0C277E01` | NOT zero |
| pixel `c0` | `(0.384, 0.800, 1.000, 0.000)` | sane, it is the fog colour |
| vertex `c8` / `c10` | `(0.3,0.001,1,1)` / `(2,2,2,1)` | sane |
| POSITION | `(85.79,-10.78,-25.38)` | sane world coords |
| TEXCOORD0/1 | `(1.34,0.36) (0.64,1.34) (-0.34,0.64)` | a rotated unit square, normal for billboards |
| tex0 `F1520000` | 128x128 k_8_8, rgb black in 14% | real picture |
| tex1 `F152C000` | 128x128 k_8_8, rgb black in 0% | real picture |

So the earlier fork ("colour bytes ZERO -> our transcode; WHITE -> the constant")
was a false dichotomy, and one half of it was never possible in the first place:
**D3DCOLOR transcodes with `kSwap32`, which is a pure byte PERMUTATION.** It
cannot turn a nonzero colour into zero. It can only produce the wrong channel
ORDER, which reads as a wrong hue, never as black. Worth remembering before
writing another "maybe the transcode zeroes it" note.

The transcode was also checked and is RIGHT. Guest dword `0x017E270C` is stored
big-endian as `01 7E 27 0C`; swapped it is `0C 27 7E 01`, and read as
`B8G8R8A8_UNORM` that is B=0C G=27 R=7E A=01 -- exactly the guest's ARGB.

**What it actually is: the quads are not being cut out.** Run
`--kameo_gfx_only_primitive=13` and look at the grass alone against the cleared
background. It is not a field of blades -- it is a mass of solid, opaque,
BLOCKY RECTANGLES with staircase silhouettes. Grass is drawn as alpha-cut cards,
so when the cutout does not happen you get the whole quad. That is a geometry /
alpha result, not a shading one, and it explains the "black quads or triangles"
shape directly.

Two more hypotheses died here, so do not re-run them either:

| hypothesis | how it died |
|---|---|
| `B8G8R8A8_UNORM` is an illegal input-assembler vertex format | ran with `--kameo_gfx_debug_layer=true`: 6257 D3D12 messages, not one about the input layout |
| the COLOR attribute never reaches the shader | `--kameo_gfx_force_vertex_color=true` visibly changed the menu highlights, so it arrives |
| ALPHA_TEST is compiled out, so `clip()` never runs | it is baked ON -- `g_SpecConstants()` returns `R11G11B10_NORMAL \| ALPHA_TEST` |

**SOLVED: the alpha test's comparison FUNCTION was never read.** Two things had
to be corrected before this was findable, and both are worth remembering.

First, `E5F318C38861EDB5` -- named here as "the best grass candidate" -- is not
what covers the screen. A census of quadlist draws by triangles submitted says
the dominant shader is `0C55435391DE66E3` at **946 draws / 307,324 triangles**
per 120 frames, against 9,490 for the old candidate. Rank the suspects before
analysing one; the per-hash dump reports whichever twelve shaders it saw first,
which is not the same thing at all.

Second, the guest's own code settles what grass even is:
`grass_coreRender` (0x824dca18) draws QUADLIST with `SetStreamSource(..., 0x14)`
-- a **20-byte** guest vertex -- and uploads `c4..c15` from its parameter block,
where `c8` is the player position. The draw being analysed had a 36-byte layout
and a fog-shaped `c8`, so it was never grass_coreRender's geometry.

The real shader is an ordinary lit one, ending in a fog lerp (the `-FogColour`
early and `+FogColour` late cancel):

```hlsl
oC0.w   = (SunLightColour.w - r3.w) * DiffuseSampler.w;
oC0.xyz = lerp(FogColour.xyz, lit, saturate(fogFactor));
clip(oC0.w - g_AlphaThreshold);
```

Measured state for those draws: **`enabled=YES func=4 ref=0.0`**, blend
`00010001`. Xenos func 4 is **GREATER**, and blend ONE/ZERO means blending is
OFF. So the guest asked for "keep only texels whose alpha is strictly above
zero" -- the standard foliage cut-out.

`CaptureState` read the enable bit and the reference value but **never the
function**, and the translated shaders implement exactly one comparison,
`clip(alpha - threshold)`, which discards when `alpha < threshold` -- GEQUAL.
With a reference of 0 that means "keep alpha >= 0", i.e. keep EVERYTHING. The
diffuse texture is 62% fully transparent and 51% black RGB, so every transparent
texel of every foliage card was drawn, opaque, in black. That is the black
quads: not shading, not vertices, not the shader translation -- a comparison
operator.

It also predicts the pixel value exactly, which is what confirms it rather than
merely fitting it. With the lit term at ~0 the output is
`lerp(FogColour, 0, ~0.96)` = `(0.384, 0.800, 1.000) * 0.041`, and the black on
screen measured **R4 G8 B10** -- ratio 1 : 2 : 2.5 against FogColour's
1 : 2.08 : 2.6.

The fix folds the function into the threshold, since the shader offers only the
one comparison: GREATER becomes GEQUAL by nudging the reference up half a texel
step (`1/512` -- alpha arrives from 8-bit UNORM textures and moves in 1/255ths,
so that sits safely between "exactly zero" and "the smallest nonzero alpha").
NEVER and ALWAYS map to thresholds outside `[0,1]`. LESS / LEQUAL / EQUAL /
NOTEQUAL cannot be expressed as a one-sided clip and pass everything rather than
cut wrongly -- none is used by Kameo so far, so if something ever draws with a
halo, look there first.

Note what this means for the "alpha testing disabled" case, which used to be
written as a threshold of 0 with a comment calling it a no-op: it is NOT a
no-op, it is GEQUAL-0, and the only reason it looked like one is that nothing
had a texture with exactly-zero alpha in front of it yet. Disabled now maps to
-1.

### Metallic / cel-shaded characters: what has been RULED OUT

Open, but a good deal of the space is eliminated. Skinned draws are identifiable
by BLENDINDICES in the declaration, which is what separates a bone-weighted mesh
from terrain and foliage.

**The character shader names its own constants, and they are NOT the foliage
shader's.** `0C55435391DE66E3` (foliage) has c0/c1/c2 = SunLightColour /
AmbientLightColour / FogColour. The character body shader
`B757C154803C2E1A` does not: it uses `c3 c_constant0` (specular exponent),
`c14 c_ambientHigh`, `c19 c_light0dir`, `c20 c_light0colour`,
`c21/c22 c_light1/2colour`, `c26 c_light1atten`, `c27 c_fillColour`,
`c28/c29 c_light1/2pos`, `c30 c_ambient`. Read the shader's own packoffset table
before labelling registers; the same index means different things per shader.

Measured on the body draws, and all HEALTHY -- so the fault is not a missing or
zeroed light constant:

```
light0 dir(0, 0.928, 0.371) col(1.600, 1.300, 1.400)   dir is unit length
light1 col(1,1,1,0.889) pos(0,0,0,1) atten(1.5, 0, 0, 1.778)
ambient(0.150, 0.150, 0.300)  ambientHigh(0.150, 0.150, 0.300, 0.75)
specExp 44.622
```

| hypothesis | how it died |
|---|---|
| a lighting constant is zero or garbage | measured, see above -- all sane |
| `c254`/`c255` (the Xenos literal pool) are never uploaded | measured: they hold real literals, e.g. `(0,1,0.333,0.5)` and `(-1,0,0,0)`, and differ per shader |
| the DEC3N -> R11G11B10 normal repack is wrong | checked field by field against `tfetchR11G11B10`: sign bits at 10/21/31, magnitudes 10/10/9 bits, scales 1024/1024/512. Exact |
| the fetch swizzle is ignored for compressed textures | it IS ignored (`ApplySwizzleToRgba8` only runs when `block == 1`) but harmless: a census over every dump shows DXT1/DXT2_3/DXT4_5 are ALWAYS swizzle 0x688, the identity |

**The prime remaining suspect is the DXN normal map.** The body shader binds
`s0` as xenos-fmt 49 (DXN) and reads only `.xy` from it -- a two-channel normal
with Z reconstructed. `MapTextureFormat` sends DXN straight to `BC5_UNORM` with
no channel handling anywhere, and Xbox 360 DXN is not byte-identical to DXGI
BC5: the two BC4 sub-blocks carry the components in the opposite order. Swapped
X and Y in a normal map gives light coming from the wrong direction per texel,
which is exactly the hard, metallic, cel-shaded look. Not yet verified -- no DXN
texture has been dumped in any session, so its swizzle is also still unmeasured.
The check is to dump one DXN texture, decode both orderings, and see which is a
plausible normal map (mostly ~(0.5, 0.5) flat with detail around it).

### Two bugs found by the D3D12 debug layer and the counter split

**A SPIR-V failure was silently dropping shaders from the cache.** The old note
here said the shaders that fail are "SPIR-V only, which the D3D12 path does not
use", and treated that as harmless. It is not. `build_shader_cache.py` kept a
shader only when XenosRecomp exited 0 AND wrote a `.meta`; the Vulkan backend
rejects some vertex shaders with *"partial explicit stage input location
assignment via vk::location(X) unsupported"*, and XenosRecomp then exits 1
without a `.meta` -- **after having already written a perfectly good `.dxil`**.
The whole entry was dropped, so those shaders were missing at runtime forever
and no amount of re-dumping and rebuilding could fix it: the containers were in
`shader_dump/` the whole time and simply never reached the cache. Verified by
hashing the re-dumped misses and grepping the generated `.cpp` -- all 13 ABSENT.
The script now salvages the DXIL (spirv size 0, spec 0; `specConstantsMask` is
not read anywhere, since the spec constants are baked at compile time).

Cost of the bug: **28 dropped draws per frame**, a family of consecutive vertex
shader objects -- which is what skinned-character VS variants look like -- and
almost certainly the "player model not fully loading in".

**The alpha blend slots were being given colour factors.** `srcBlendAlpha` /
`dstBlendAlpha` went through the same `HostBlend()` as the colour ones, which
can return `SRC_COLOR` / `DEST_COLOR`. D3D12 forbids those in the alpha slots and
rejects the entire blend state -- *"DestBlendAlpha[0] is trying to use a
D3D11_BLEND value (0x3) that manipulates color, which is invalid"* -- and a
rejected blend state is completely silent without the debug layer attached.
Xenos lets those fields name a colour and uses that source's alpha, so
`HostBlendAlpha()` now folds `SRC_COLOR -> SRC_ALPHA` and friends.

**Split the "no shader" counter three ways.** `untranslated` used to also absorb
"colour draw with ps_object == 0 outside a shadow pass", which needs the
opposite fix. The scene line now reads `[N untranslated, N null-ps, N no-vs]`,
and a genuine cache miss names the guilty stage and its guest container. That
split is what proved these were all missing VERTEX shaders with the pixel shader
present -- the thing that pointed at the cache build rather than the draw path.

### Diagnostic cvars added for this (all default OFF)

* `--kameo_gfx_skip_primitive=N` -- drop every draw of guest primitive N.
* `--kameo_gfx_only_primitive=N` -- draw ONLY primitive N. The more useful of
  the two: a suspect primitive alone against the cleared background shows its
  real shape and coverage, which is invisible under the rest of the scene. This
  is what turned "black grass" into "opaque un-cut rectangles" in one screenshot.
* `--kameo_gfx_force_vertex_color=true` -- overwrite every COLOR element with
  opaque white after the transcode. Answers whether the attribute REACHES the
  shader, which the vertex bytes alone cannot.

Note these drop draws by design, so the menu, start text and book pages will
look broken while they are on. That is the tool working, not a regression.

### The freeze: a 2x buffer overrun on 16-bit packed textures

`DXGI_ERROR_INVALID_CALL` -> device removed -> the picture freezes while the
renderer carries on presenting. The debug layer named it exactly:

```
CopyTextureRegion: PlacedFootprint extends past the end of the buffer.
RowPitch 512, Height 64, Format R8G8B8A8_UNORM -> requires 32272 bytes;
but the buffer only has 16384 bytes.
```

The upload describes its row pitch in TEXELS, as `upload_pitch / block_bytes *
block`. The `byte_channels` branch updates `host.block_bytes` to 4 after
expanding to RGBA8; **the `expand_to_rgba8` branch did not**. So a k_5_6_5
texture kept `block_bytes = 2` while its data was already 4 bytes per texel,
the footprint claimed 256/2 = 128 texels per row = 512 bytes, and the buffer had
been allocated for 256. Exactly 2x, every time, for every 16-bit packed format
(k_5_6_5, k_1_5_5_5, k_4_4_4_4). k_2_10_10_10 was right by luck -- its
block_bytes is 4 already.

It looked like a crash tied to a POINT IN THE GAME rather than to a format
because those formats are rare here: a whole session's census contains a single
k_5_6_5, so it fired the first time that one texture was uploaded. Chasing the
game event would never have found it; the debug layer named it in one line.

Fixed by setting `host.block_bytes = 4` alongside the format. Verified by
watching the exact texture (`F1607000`, 4x64, fmt 4) upload with zero
CopyTextureRegion errors and zero device removals.

**Also fixed here: resolve targets are now keyed on address AND SHAPE**
(`ResolveKey{address, width, height, depth}`). Keying on the address alone had
caused five bugs; the last was EFDEC000 thrashing between 1280x720 and a parade
of small sizes many times a frame, rebuilding the texture behind a live heap
slot each time. Rebuild-in-place is gone entirely -- incompatible uses can no
longer collide, so nothing is pulled out from under a draw that already sampled
it. Costs nothing measurable: a session settles at ~64 distinct targets.

### The missing HUD: BeginVertices was counted and thrown away

Health bar, energy bar and the warrior button prompts were absent while the
score and combo text rendered fine. The split is the giveaway: text comes
through `DrawVerticesUP`, which is captured; the 2D icon system does not.

`IconPlayerDrawBars` and `IconPlayerRenderButtonTexture` both go through
`iconRenderIcon2DTrueXYScale` (0x822d13c8), which draws with
`D3DDevice_BeginVertices(dev, D3DPT_TRIANGLEFAN, 4, 0x18)`. The hook for that
existed but only ran `CountDraw` and some logging -- it never captured anything.
The census says **~380 BeginVertices a frame against 130 DrawVerticesUP**, so
roughly **250 standalone 2D draws a frame were being dropped outright**.

Capturing them needs care, because `BeginVertices` hands the guest a pointer
into a ring and RETURNS -- the vertices do not exist yet -- and the matching
EndVertices is INLINED (`*(_DWORD *)D3D__pDevice = *(_DWORD *)(D3D__pDevice +
13620)` at the end of the icon function), so there is nothing to hook on the far
side. Deferring the whole capture to the next call does not work either: each
icon binds its texture BEFORE its own BeginVertices, so by the next one the
state already belongs to the next draw.

So the two halves are taken at different times: **state at BeginVertices, where
it is still this draw's, and the geometry at frame end**, by which point the
guest has filled the ring. The DrawCall is pushed at capture time so ordering
is preserved, and `SubmitCapturedFrame` transcodes the deferred rings before the
frame is handed to the render thread -- the arena stays self-contained.

**One thing this switched on that was not wanted yet.** With those draws
captured, the post-process composite appears: a full-screen OPAQUE quad
(`blend 00010001`, `vp 1280x720`) sampling the scene resolve. It samples the
bloom chain, which is still black, so it painted a large dark rectangle over the
scene. The downsample steps are fine -- their viewports (426x240, 160x90) differ
from the guest size so they route to offscreen targets normally; it is only the
full-size composite that lands on the screen. Held back for now, narrowly:
deferred + full-screen + opaque, which no HUD element ever is. Before this work
it was invisible, and invisible is closer to right than black. Remove the hold
when the bloom chain works.

`--kameo_gfx_capture_begin_vertices=false` restores the old behaviour.

### Performance: the transcode was being redone every frame

Measured, not guessed. `--kameo_gfx_only_primitive=99` captures everything and
submits nothing, which splits CPU capture from GPU work in one run:

| configuration | frame time | fps |
|---|---|---|
| full rendering | 77 ms | ~13 |
| capture only, no uploads or draws | 28.6 ms | ~35 |

So the CPU capture ALONE capped the game at 35 fps. Timers inside the capture
then split that number:

```
vertices 1817 ms (1160 MB), constants 177 ms, indices 0 ms, restream-hash 3 ms
```

per 120 frames -- 15 ms a frame transcoding vertices, and up to 33 ms and 24 MB
in heavier scenes. Everything else was noise.

The cause: `g_vertexReuse` is cleared every frame along with the arena, so every
vertex in the scene was re-transcoded from guest memory 60 times a second even
though level geometry never changes.

Two changes, in order of payoff:

1. **A cross-frame transcode cache** (`g_transcodeCache`), validated by the same
   sampled content hash the per-frame path already used, evicting anything
   unused for 240 frames. A hit still MEMCPYs into the frame arena rather than
   handing the submit path a pointer into the cache -- the arena is moved to the
   render thread while the guest thread captures the next frame, so a DrawCall
   pointing into a live cache would be a data race. A memcpy is ~10x cheaper
   than the transcode and keeps the frame self-contained.
   **Result: vertices 4017 ms -> 13 ms per 120 frames, ~85% hit rate, 26 MB
   live. fps 13 -> 19-31.**

2. **The op switch was hoisted out of the per-vertex loop.** It used to run per
   vertex per element. Worth about 10% per byte on its own -- real but small,
   because the loop is memory-bound rather than branch-bound. Do not expect much
   from micro-optimising it further; the volume was the problem.

**Next lever:** constants are now the largest capture item at ~4.7 ms a frame
(4 KB copied and byte-swapped per stage per draw, deduplicated only against the
immediately preceding block). After that it is the submit/GPU side, still
roughly 48 ms a frame with ~2600 draws and ~278 depth-only shadow draws.

### Performance findings

**Upload dedup matters more than ring size.** Identical constant blocks and
vertex windows were re-uploaded once per draw — thousands of copies of the same
4 KB. Deduplicating by arena offset took the constant ring from exhausted to
8 MB of 96.

**Transcode whole vertex buffers, not per-draw windows.** Kameo packs many
meshes into one buffer and draws them as chunks with overlapping index ranges,
so per-draw windows re-copied the same buffer repeatedly: 64 MB a frame, ring
exhausted, ~1600 draws dropped. One copy per buffer per frame is smaller and
cheaper, and indices stay usable as-is. Result: 8.8 MB, zero dropped.

**Vertex reuse must NOT key on the source pointer for `DrawVerticesUP`.** Its
data arrives in a scratch buffer the guest rewrites before every draw, so the
same pointer/count/layout describes completely different vertices. Reusing on
that key collapsed all ~148 glyphs of a screen's text into one quad — 148 draws
submitted, vertex ring reporting 0 KB.

**Scale the guest viewport to the swap chain.** The guest renders at 720p; using
its numbers verbatim pins every draw into a 1280x720 rectangle in the corner of
a larger window. Invisible at a 720p client area, glaring the moment it is not,
because the Bink quad is full-screen clip space.

### Diagnostics worth keeping

* **A VEH crash handler** that symbolises the faulting frame. The watchdog only
  reports STALLS; a hard fault is the opposite, and used to kill the process
  silently. This found the plume PSO trap in one run.
* **`--kameo_gfx_debug_layer=true`** attaches the D3D12 info queue. plume only
  wires one up in its own debug builds, and PSO rejection is otherwise
  completely silent. This identified the `lib_6_3` problem.
* **`--kameo_gfx_dump_shaders=true`** writes every container, not just
  unresolved ones — needed because once a cache exists almost nothing misses, so
  "dump the misses" collects a dozen files instead of the ~1000 the cache was
  built from.
* The **magenta fallback texture** at heap slot 0. White was inconclusive
  (plausible-looking); magenta answers "is this texture missing?" from one
  screenshot.
* The **all-zero texture check**, which is what proved the resolve targets were
  the cause rather than shading.

### Still to do

1. **Depth resolves.** `Flags & 7 == 4` is skipped, so the three `k_24_8`
   shadow maps stay empty (~123k lookups/frame). Needs a sampleable depth copy.
2. **Bindless heap eviction.** 4096 slots, no eviction. The boundary is checked
   and falls back to the placeholder instead of corrupting neighbouring
   descriptors, but a long session still runs out.
3. **Shader coverage grows with where the game has been.** The cache is now
   ~2478 shaders (was 1038) with ZERO DXIL failures. The ones that fail are
   SPIR-V only -- but do NOT read that as "harmless", which is what the old note
   here said. Until the salvage fix above they were dropped from the cache
   ENTIRELY, DXIL and all, and cost 28 draws a frame. Misses are dumped to `shader_dump/`
   automatically, so the loop is: play new content, re-run
   `build_shader_cache.py`, rebuild. Note `capture dropped N shader` in the
   scene line accumulates over 120 FRAMES, not one -- divide before comparing.
4. **EDRAM predicated tiling** — still collapsed by ignoring it; a seam is
   visible along the left edge of the menu background.
5. **Retire the legacy 2D overlay path** (`kameo_gfx_legacy_overlay`, default
   true) once the general path is confirmed at least as good everywhere.
