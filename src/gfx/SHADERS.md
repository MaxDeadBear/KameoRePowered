# Kameo shader corpus and container format

Notes for porting [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (vendored
at `thirdparty/XenosRecomp`) to Kameo. Derived from the retail vanilla image
`assets/default.xex`; TU addresses differ.

## Container version: 0x0E, not 0x11

XenosRecomp targets Sonic Unleashed, whose shader containers are stamped
`0x102A11xx`. **Kameo's are `0x102A0Exx`** — the same container family from an
older XDK shader compiler.

The low byte is the shader stage:

| magic | stage |
|---|---|
| `0x102A0E00` | pixel |
| `0x102A0E01` | vertex |

No other low-byte values occur anywhere in the image, so the discriminator is
clean.

## What transfers unchanged

The first three dwords match `ShaderContainer` exactly:

```
0x00  flags          (0x102A0Exx)
0x04  virtualSize
0x08  physicalSize
```

Verified by segmentation: scanning the whole image for the magic yields 145
containers, and for all 144 consecutive pairs
`addr + virtualSize + physicalSize <= next_addr`, with zero violations. If these
fields were misidentified the containers would overlap.

So XenosRecomp's *discovery* loop in `main.cpp` works for Kameo with two edits:

1. change the magic compare from `0x102A1100` to `0x102A0E00`
2. drop the `field1C == 0 && field20 == 0` heuristic — it is a v0x11 artifact
   and does not hold here (`texturequadpsPixelShader` has `4` at `+0x1C`)

The `XXH3_64bits(container, virtualSize + physicalSize)` keying also transfers,
which is what lets the runtime hash the guest blob at `CreatePixelShader` time
and look up a precompiled result.

## The container header: v0x0E is v0x11 minus one dword

`D3DDevice_CreatePixelShader` (0x820C8360) and `D3DDevice_CreateVertexShader`
(0x820C86D0) settle the first three fields directly — they are the only fields
the runtime itself touches:

```c
v2 = *pFunction & 0x7F;       // 0 = pixel, 1 = vertex (selects a 52 or 592 byte host object)
v3 = pFunction[2];            // physicalSize
v5 = pFunction[1];            // virtualSize
v7 = (char*)pFunction + v5;   // microcode blob starts here, length physicalSize
```

So **the ucode lives at `container + virtualSize` for `physicalSize` bytes.**
Everything before that is the virtual/header portion.

The remaining offsets were placed by observing that two pixel shaders contain
the same 9-dword structure at different positions — `texturequadps` at `+0x18`
and `depthTexturePS` at `+0x4C` — which are precisely their `dw5` values:

```
texturequadps  @0x18: 10020000 00000004 0 0 00000821 0 00010001 00000001 00003050
depthTexturePS @0x4C: 18020200 00000004 0 0 00000821 0 00010001 00000011 00003050
```

`depthTexturePS` is also the sample with a non-zero `dw3` (`0x18`), its constant
table. That gives:

```
0x00  flags                  (0x102A0Exx; low byte = stage)
0x04  virtualSize
0x08  physicalSize
0x0C  constantTableOffset
0x10  definitionTableOffset
0x14  shaderOffset
```

Six dwords — **v0x11's layout with the `fieldC` dword removed**, everything
after it shifted one slot earlier. That single missing dword explains the whole
discrepancy, including why the `field1C == 0` heuristic misfires: Kameo's `dw7`
is not a reserved zero, it is the second field of the `Shader` struct.

### Validation

Scanning all 145 containers in the image and testing the hypothesis:

| check | result |
|---|---|
| `0 < shaderOffset < virtualSize`, 4-aligned | 145 / 145 |
| `constantTableOffset` in range when non-zero | 56 non-zero, 0 violations |
| `definitionTableOffset` in range when non-zero | 20 non-zero, 0 violations |
| smallest offset observed (implies header size) | `0x18` |

Zero violations. `shaderOffset` distribution: `0x18` (79), `0x4C` (22), `0x5C`
(8), `0x60` (4), `0x80` (4), `0xD4` (4).

### Header offsets confirmed from the runtime

Two of the three interior offsets are now pinned by code, not inference. Both
creators copy the container into the host shader object (pixel at +52, vertex at
+592), so host-relative reads map back to container dwords:

| function | reads | resolves to |
|---|---|---|
| `XGRegisterVertexShader` | `pShader[25].ReadFence` = +612 = 592+20 | container **dw5 = shaderOffset** |
| `D3DDevice_SetPixelShader` | `pShader[2].Identifier` = +64 = 52+12 | container **dw3 = constantTableOffset** |

`XGGetMicrocodeShaderParts` independently re-confirms dw1/dw2:
`pPhysicalPart = pFunction + dw1`, `cbPhysicalPartSize = dw2`.

### The VertexShader element array

`XGRegisterVertexShader` walks it as `v5 + 4*v5[6] + 40`, then loops `v5[7]`
times masking each entry with `& 0xFFF` to index 12-byte records in the physical
part. So relative to the `Shader` struct:

| dword | field | vs XenosRecomp |
|---|---|---|
| `s[6]` | `field18` (skip count) | same |
| `s[7]` | `vertexElementCount` | same |
| `s[10]` | element array start | **XenosRecomp uses `s[9]`** |

Confirmed on data: `BlendFlameVSHDX9VertexShader` has `field18=1`,
`vertexElementCount=4`, and `s[11..14]` = `00100004 0000A005 00005006 00315007`
— sequential low-12-bit addresses, exactly what the `& 0xFFF` indexing wants.
Interpolators follow at `s[10 + field18 + vertexElementCount]`.

### The constant table is a register patch list

`SetPixelShader` treats `container + constantTableOffset` as: a 24-byte header
with a byte size at `+16`, then records of `{u16 dstOffset, u16 dwordCount}`
followed by payload, `memcpy`d to **`device + 0x480`** — the Xenos register
shadow (see `kameo_guest_device.h`). A second pass applies AND/OR mask pairs
instead of raw copies. This is how a shader installs its own fetch constants and
register defaults at bind time.

### There is no physicalOffset — ucode starts at the physical part

`D3D::LazyWriteShaders` (0x820D4BA0) programs both stages into the command
buffer and settles this. It first locates each Shader struct, which also
confirms `dw5 = shaderOffset` for pixel shaders as well as vertex:

```c
v6 = *(a1 + 12948);                     // bound vertex shader object
v7 = *(a1 + 12944);                     // bound pixel shader object
vtxShader = *(v6 + 612) + v6 + 592;     // 592 = container base, 612 = 592+20 -> dw5
pixShader = v7 + v7[18] + 52;           //  52 = container base,  72 = 52+20  -> dw5
```

Then it emits the GPU address and length:

```c
v15 = *(v6 + 40);                       // vertex ucode ptr, cached by XGRegisterVertexShader
*v25++ = (((v15 >> 20) + 512) & 0x1000) + (v15 & 0x1FFFFFFF);
*v25   = *(v6 + 600) >> 2;              // 600 = 592+8 -> container dw2 = physicalSize

*... = ((((v7[3] >> 20) + 512) & 0x1000) + (v7[3] & 0x1FFFFFFE)) | 1;  // pixel ucode ptr (host +12)
*... = v7[15] >> 2;                     // 60 = 52+8 -> container dw2 = physicalSize
```

Both stages take the address straight from the allocated physical copy and the
length straight from `physicalSize`. **Nothing is added.** So for Kameo:

```
ucode      = container + virtualSize
ucodeBytes = physicalSize
```

In `shader_recompiler.cpp:1494`, treat `shader->physicalOffset` as **0**.

### What s[0] and s[1] actually are

They are the two shader control registers, not offset/size.
`IncrementalShaderPatchAndLoad` types its argument `union GPU_PROGRAMCONTROL *`,
and `LazyWriteShaders` ORs the vertex and pixel `s[0]` together into
`a1 + 11616` and their `s[1]` into `a1 + 11620` before writing them out. That is
why `s[0]` reads as packed bitfields (`0x10020000` / `0x18020200` /
`0x00310004`).

`s[1]` passing an "always `< physicalSize` and 4-aligned" filter across 75
samples was coincidence — small register values look like small offsets. Worth
remembering as a caution against identifying fields statistically.

### s[4] is interpolatorInfo; s[7] confirmed for both stages

`s[7]` holds `vertexElementCount` for vertex shaders and `outputs` for pixel
shaders, exactly as v0x11 has it. Two independent confirmations:

* `XGRegisterVertexShader` loops `s[7]` times over the element array, and
  `BlendFlameVSHDX9VertexShader` has `s[7] = 4` with four elements.
* `LazyWriteShaders` tests pixel `s[7] & 0x10` (`PIXEL_SHADER_OUTPUT_DEPTH`) to
  decide a depth-mode register, and `depthTexturePSPixelShader` — a shader whose
  name says it writes depth — has `s[7] = 0x11` (COLOR0 | DEPTH).

Across the 93 pixel shaders `s[7]` is `{0x01: 86, 0x21: 4, 0x11: 2, 0x31: 1}`:
only COLOR0, DEPTH and one further bit ever appear, never COLOR1/2/3, so Kameo
does not use MRT.

`interpolatorInfo` moves from `s[5]` to **`s[4]`**. The evidence is semantic, not
statistical: a matching vertex/pixel shader **pair** always shares the same
`s[4]` value — `effectsScreenDropletVSHDX9VertexShader` and
`effectsScreenDropletPSHDX9PixelShader` are both `0x2C84`; `unk_8203E368` (PS)
and `unk_8203E600` (VS) are both `0x3C84`. That is exactly the constraint an
interpolator count must satisfy, since a vertex shader's outputs have to match
its pixel shader's inputs. `(s[4] >> 5) & 0x1F` then yields 1..5 across the
corpus, and `s[5]` is zero in all 145 shaders.

This is a stronger class of evidence than the `s[1]` filter that misled us
earlier — pairing is a semantic invariant, whereas "looks like a small offset"
is satisfied by any small register value.

## Pipeline status

**All 145 shaders translate to HLSL and compile to DXIL + SPIR-V with zero
errors**, measured one shader per process.

A whole-directory batch aborts. This is a **harness defect, not a shader
defect** — every shader compiles cleanly in a process of its own, and DXC
reports zero errors right up to the crash.

What has been ruled out:

| hypothesis | test | result |
|---|---|---|
| a specific bad shader | per-shader runs | all 145 pass |
| concurrency | `par_unseq` -> `seq` | still crashes |
| `DxcCompiler` instance reuse | construct one per shader | still crashes |
| `args[32]` stack overflow | counted `argCount++` sites | max ~8, not an overflow |
| DXIL-specific | `-DXENOS_RECOMP_DXIL=OFF` (SPIR-V only) | still crashes |
| transient / retryable | 8 consecutive full runs | 8 failures |

It is **nondeterministic**: the same 12 shaders passed 5 of 6 runs and crashed
once. Batch size correlates but not monotonically (10, 16, 18 pass; 6, 8, 12,
14, 20 fail). That signature — nondeterministic, worsening with volume,
surviving serialisation — is heap corruption, and since each shader triggers two
DXC invocations (DXIL + SPIR-V, ~268 for the full set), the most likely culprit
is DXC itself being driven many times in one process.

**Resolved by process isolation plus retries.** `main.cpp` gained a `--blobs`
mode that compiles a single container and writes `<out>.dxil`, `<out>.spirv` and
a `<out>.meta` line (`hash dxilSize spirvSize specConstantsMask`), and
`scripts/build_shader_cache.py` drives one process per shader and merges the
results.

Isolation alone is not sufficient: even a single-shader process fails
intermittently (one shader measured 12 successes in 15 runs), and rapid
back-to-back launches appear to make it worse. The retry loop absorbs this.

## Building the cache

```
python scripts/build_shader_cache.py \
    --exe   thirdparty/XenosRecomp/build/XenosRecomp/Release/XenosRecomp.exe \
    --image <decompressed image>.bin \
    --out   src/gfx/shader_cache.cpp \
    --retries 20
```

Current result: **145 / 145 compiled, 134 unique after hash dedup.** Built and
committed as `src/gfx/shader_cache.cpp` (2.5 MB), with `src/gfx/shader_cache.h`
hand-written alongside it -- XenosRecomp emits the .cpp but not the header, so
the struct layout has to match the initialiser order the generator writes.
Verified by compiling and linking it standalone: 134 entries, DXIL 662408 bytes,
SPIR-V 172027.

### The translated shaders are BINDLESS -- this is the real cost of using them

Having the cache is not enough to draw. The translated shaders declare a binding
model that the renderer has to provide exactly, and it is not the simple
descriptor-set model the Bink and overlay paths use.

From `shader_common.h` and `shader_recompiler.cpp`:

```hlsl
Texture2D<float4>   g_Texture2DDescriptorHeap[]   : register(t0, space0);
Texture3D<float4>   g_Texture3DDescriptorHeap[]   : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState        g_SamplerDescriptorHeap[]     : register(s0, space3);

cbuffer VertexShaderConstants : register(b0, space4);
cbuffer PixelShaderConstants  : register(b1, space4);
cbuffer SharedConstants       : register(b2, space4);
```

**Unbounded descriptor arrays**, indexed per-sample by a descriptor index the
shader reads out of its own constant buffer. The SPIR-V path goes further and
uses push constants holding 64-bit *buffer device addresses*, loading constants
via `vk::RawBufferLoad`.

So drawing with these shaders needs:

1. bindless descriptor heaps for textures (three spaces) and samplers
2. three constant buffers in space4 -- the 256-float4 vertex and pixel blocks
   copied from `0x780` / `0x1780`, plus a shared block whose layout is fixed by
   `DEFINE_SHARED_CONSTANTS()` at `packoffset(c16)`
3. **texture descriptor indices written into the constant data**, because that is
   how the shader finds its textures -- binding a texture to a slot is not
   enough

That third point is the one that catches people: the guest's texture bindings
(`device + 12704 + 4*sampler`) have to be turned into heap indices and patched
into the constants before each draw. UnleashedRecomp does exactly this.

None of that infrastructure exists here yet -- the Bink and overlay pipelines use
small fixed descriptor sets. This is the single largest remaining piece of the
general draw path, larger than the geometry plumbing.

### Wiring the cache in: hook shader CREATION, never the bound object

The obvious approach -- take the bound shader at draw time and hash its ucode --
**cannot work for vertex shaders**, and the reason is already recorded above: no
vertex shader reaches the GPU as authored, because `XGRegisterVertexShader`
patches vertex fetch into it per declaration. Confirmed by dumping the bound
object at a draw:

```
bound vertex shader object = 40424CD0
  00:00000000 04:00000003 08:00000000 12:FFFFFFFF 16:00000000 20:00000000
  40:EEBA6480  <- the only pointer; first dwords 70153003 00001200 C4000000
```

`BaseFlush` (+20) is **zero**, so shaders do not follow the texture pattern of
carrying their data address there. The pointer at +40 is the *patched* runtime
ucode, and it carries no container magic -- hashing it will never match a cache
entry.

Hook `D3DDevice_CreateVertexShader` (0x820C86D0) and
`D3DDevice_CreatePixelShader` (0x820C8360) instead. Their argument is the
**authored container**, which is exactly what the cache was built from, and the
return value is the object that later shows up bound at `device + 20440`. So
capture the association at creation:

| field | where |
|---|---|
| container base | `pFunction` (r3) |
| `virtualSize` | `pFunction[1]` |
| `physicalSize` | `pFunction[2]` |
| ucode | `pFunction + virtualSize`, `physicalSize` bytes |

and the cache key is

```
hash = XXH3_64bits(container, virtualSize + physicalSize)
```

taken over the RAW guest bytes -- read the two sizes byte-swapped, but hash the
bytes as stored. The cache was built from the dumped image in that same order,
so the two agree. `g_shaderCacheEntries` is sorted by hash, so look up with
`lower_bound` (UnleashedRecomp's `FindShaderCacheEntry` does exactly this).

Store `guest shader object -> ShaderCacheEntry*` at creation and the draw path
becomes a pointer lookup with no hashing per draw.

### Getting the decompressed image: dump it from a running build

The script needs the DECOMPRESSED image and the shipping `assets/default.xex` is
compressed, so a raw scan of the XEX finds zero containers. There is no
decompressor to hand: `rexglue` has no such subcommand (only `codegen`, `init`,
`recompile-tests`) and XenosRecomp takes an image rather than a XEX.

Do not write an XEX2/LZX decompressor. **The runtime has already decompressed
it** -- it maps the module at `82000000-82BF0000`, the range the function table
reports at startup. `DumpGuestImageOnce` in `kameo_gfx_hooks.cpp` writes that
range to `guest_image.bin` on the first present:

```
out/build/native/guest_image.bin   12,517,376 bytes
```

Byte order needs no fixing -- this file records that the `.xsh` blobs matched the
XEX blobs with no swapping. Confirmed good by scanning it for the pixel-shader
magic `0x102A0E00`: **93 hits, exactly the 93 pixel shaders** counted below.

Note it hangs off the **Present** hook, not Swap. Kameo drives frames through
`D3DDevice_Present` (0x820D0048); `D3DDevice_Swap` is never reached in this
title, so a dump placed there silently never runs.

`--retries 8` left one shader unbuilt; 20 is reliable. The script re-implements
container discovery itself, so it takes the decompressed image directly rather
than a directory of pre-split files.

Install the `zstandard` Python module to get compressed caches; without it the
script still works but emits the blobs uncompressed (662 KB DXIL + 172 KB
SPIR-V, which is a 2.5 MB generated .cpp).

### Measure with absolute paths

Earlier readings of "141/145" and "143/145" in this file were wrong. The harness
running these commands resets the working directory between invocations, so a
relative `./build/.../XenosRecomp.exe` sometimes resolved from the repo root and
the binary simply was not found. That yields **exit 127** and leaves a stale
output file, which is easy to misread as a crash. Always invoke the exe and the
`shader_common.h` include by absolute path.

### Getting it to build and run

* `thirdparty/dxc-bin` ships prebuilt DXC, so `cmake -S . -B build` +
  `cmake --build build --config Release` is enough — no external SDK.
* `pch.h` used `__builtin_bswap*`, which MSVC lacks (upstream only ever builds
  with Clang). Patched to `_byteswap_*` under `_MSC_VER && !__clang__`.
* **The input must be the DECOMPRESSED image.** `assets/default.xex` is 3.6 MB
  compressed against a ~11.9 MB loaded image, and scanning the raw file finds
  zero containers. Dump `0x82000000..0x82BF0000` from a loaded IDB (or any
  equivalent unpacker) and point the tool at a directory containing that.
* Upstream `main.cpp` asserts DXC succeeded; in Release the assert vanishes and
  the next line dereferences the null blob. Now counted and skipped.

### The definition table had the wrong shape (root cause of every crash)

`definitionTableOffset` (dw3) records are **not** v0x11's
`{registerIndex, count, physicalOffset}`. Kameo's are
`{u16 dstByteOffset, u16 dwordCount}` followed by `dwordCount` **inline**
dwords, and the header is one dword longer (size at `+16`, records at `+24`,
per `D3DDevice_SetPixelShader`).

`dstByteOffset` is relative to the register-shadow base, not a register index:
`0x22F0` -> `0x480 + 0x22F0 = 0x2770` -> pixel register
`(0x2770 - 0x1780) / 16 = 255`.

Parsed with the v0x11 structs it produced garbage indices and invalid HLSL
identifiers like `int4 i-1036 = ...`. This single misparse accounted for **all
four** translation crashes, including `unk_82065180`, which had looked like a
separate defect.

Baking these values into the shader is disabled (`kBakeDefinitionTable`), and
that is closer to hardware behaviour than a workaround: `SetPixelShader` replays
exactly these records into the guest register shadow at bind time, and the
renderer uploads that shadow wholesale, so the shader can just read `cN` from
the constant buffer.

**The renderer must therefore replay definition tables into the register shadow
when binding a shader.** That is now a runtime requirement, not an offline one.

### Kameo-specific limits that upstream gets wrong

| upstream | Kameo | why |
|---|---|---|
| pixel shaders get 224 float4 registers | **256**, both stages | the device stores pixel constants at `0x1780` and bools at `0x2780`, exactly `0x1000` = 256*16 apart, and shaders really do reference `c255` |
| 32 temp registers | **64** | Xenos allows 64 GPRs and Kameo shaders use `r63` |
| `tfetch1D` / `_Texture1DDescriptorIndex` emitted | **sampled as 2D with y=0** | neither exists: `shader_common.h` has no `tfetch1D` and `TEXTURE_DIMENSIONS` has no `"1D"`, so nothing declared them. Adding a fourth descriptor heap would push `DEFINE_SHARED_CONSTANTS` off `packoffset(c16)`; on Xenos a 1D texture is a 2D texture of height 1, so this is equivalent and reuses the existing heap |

### 114 of 133 had no reflection table — solved with a register-file fallback

When `constantTableOffset` is zero the recompiler now declares the entire
register file instead of bailing: `c0..c223/255`, `s0..s15`, `b0..b15`, in both
the SPIR-V and DXIL branches. The expression emitters already fell back to
exactly those names (`c{reg}`, `s{constIndex}`, `b{boolAddress}`), so no emitter
changes were needed — this is the fallback the upstream README suggests.

It also suits the port: the renderer copies constants wholesale out of the guest
device (`0x780` / `0x1780`) rather than resolving them by name.

**These declarations are emitted unconditionally, not only when reflection is
absent** — registers already named by reflection are skipped. That distinction
matters: a shader *with* a constant table can still reference registers outside
it, because the definition table writes `c255`. Lines mixing both, like
`closeTint.w + c255.x`, are common, and only declaring the fallback registers
for unreflected shaders left those undefined.

### Historical: the blocker before the fallback

This is exactly the limitation the upstream README calls out — "Constant buffer
registers are populated using reflection data embedded in the shader binaries.
If this data is missing, the recompiler will not function." Kameo compiled most
of its shaders without a `D3DXSHADER_CONSTANTTABLE`.

The README also names the fix: define a `float4` array covering the whole
register range and index it directly, instead of building named constant
buffers from reflection. That suits this port well, because the renderer copies
constants wholesale out of the guest device anyway (`0x780` / `0x1780`, see
`kameo_guest_device.h`) rather than by name. **This is the next substantial
piece of work.**

Until then the recompiler bails out early via `ShaderRecompiler::noConstantTable`
(upstream would have segfaulted, since a null `constantTableOffset` aliases the
container header).

### Known failures

* `unk_82065180` — the only shader of 145 that crashes *translation* itself
  (all others translate cleanly in single-file mode). It does have a reflection
  table. Currently quarantined; not yet diagnosed.
* 9 shaders translate to HLSL but fail DXC, with errors referencing declared-
  but-problematic temporaries (e.g. `r25`). Likely genuine translation bugs
  rather than container-format issues.

### Still unvalidated

`Shader::fieldC` (the svPos register, `(fieldC >> 8) & 0xFF` in v0x11) is
unverified — `s[3]` is zero in every sampled shader, so it may live elsewhere in
this version. `DefinitionTable` is also still a v0x11 shape. Neither blocks a
first translation run.

## Corpus size and where the shaders live

| source | count |
|---|---|
| static containers in `default.xex` | 145 (93 pixel, 52 vertex) |
| of those, PDB-named | 135 |
| containers found anywhere under `assets/` | **0** (491 files scanned) |
| distinct ucode blobs in xenos' shader cache | **1254** |

Every static shader is in the executable. **But "static" is the operative word,
and the conclusion originally drawn from this table was wrong** — see the
correction immediately below.

### CORRECTION: most shaders are NOT static, they come from packed game data

The row "containers found anywhere under `assets/`: 0 (491 files scanned)" was a
**raw byte scan**, and the game's data is packed — so a scan could not have found
containers even though they are there. Measured at runtime with a hook on
`D3DDevice_CreateVertexShader` / `CreatePixelShader`:

```
MISS: container 40216810 (OUTSIDE image) magic 102A0E00 vsize 360 psize 120
MISS: container 40BE202C (OUTSIDE image) magic 102A0E00 vsize 536 psize 300
```

Every unresolved container is a **valid container** (correct `0x102A0Exx` magic)
sitting in the heap, decompressed out of game data at load time. Against the
134-entry static cache the hit rate is roughly **50 hits to 1326 misses** — and
the ~50 hits line up with the "48 pixel shaders present verbatim at runtime"
figure above, i.e. the static cache covers essentially just the engine's built-in
shaders.

**So a cache built by scanning the XEX can never be complete for this title.**
Reaching only the title screen already captures **939 unique containers**, versus
145 static.

The capture point is the creation hook, and the corpus it produces is directly
usable: `RegisterShader` in `kameo_gfx_hooks.cpp` writes each unresolved
container to `shader_dump/<hash>.bin`, which is exactly XenosRecomp's `--blobs`
input format. Verified end to end on a captured container:

```
XenosRecomp.exe shader_dump/0003FC27DBE07C49.bin <out> shader_common.h --blobs
  -> dxil 5408, spirv 2714, meta hash 3FC27DBE07C49
```

The meta hash matches the filename, which confirms the **runtime hash is
identical to XenosRecomp's cache key** — `XXH3_64bits(container, virtualSize +
physicalSize)` over the raw guest bytes. So the cache and the runtime lookup
agree by construction.

Building a complete cache therefore means playing through to collect containers,
then translating the captured set rather than the XEX. Point XenosRecomp at
`default.xex` only for the built-in shaders.

### Result: 134 -> 1060 shaders, and a 99% hit rate

`build_shader_cache.py` now takes `--containers <dir>` alongside `--image` and
merges both (duplicates hash identically and dedup collapses them):

```
python scripts/build_shader_cache.py \
    --exe        thirdparty/XenosRecomp/build/XenosRecomp/Release/XenosRecomp.exe \
    --image      out/build/native/guest_image.bin \
    --containers out/build/native/shader_dump \
    --out        src/gfx/shader_cache.cpp --retries 20
```

| | before | after |
|---|---|---|
| unique shaders in cache | 134 | **1060** |
| runtime resolution to the title screen | 50 hit / 1326 miss | **2775 hit / 25 miss** |
| DXIL / SPIR-V blob size | 662 KB / 172 KB | 6.85 MB / 3.95 MB |

**8 of 1084 containers failed**, all with the same SPIR-V-only diagnostic:

```
error: partial explicit stage input location assignment via vk::location(X) unsupported
```

That is a Vulkan-path limitation, not a bad shader -- those eight compile to DXIL
fine. The script currently drops a shader entirely when the process fails, so on
a D3D12-only build they are lost for no reason. Worth making the failure
per-backend rather than per-shader.

Coverage grows with what the play session reaches, so re-run the capture after
visiting new areas and rebuild. The remaining handful of misses per run are those
eight plus occasional new containers.

### The 145 -> 1254 expansion is vertex shader patching

Comparing the 145 static ucode blobs against the 1254 blobs xenos actually saw
(`cache/shaders/shareable/4D5307D2.xsh`, Xenia `XESH` storage):

| stage | static | present verbatim at runtime |
|---|---|---|
| pixel | 93 | 48 |
| vertex | 52 | **0** |

Pixel shaders reach the GPU as authored. **No vertex shader does** — not even as
a prefix of a runtime blob, so they are rewritten rather than merely extended.
That is D3D patching vertex fetch into the microcode per vertex declaration
(`XGRegisterVertexShader`, called only from `D3DDevice_CreateVertexShader`, plus
`CMicrocodeBuilder::SetVSOutputDeclarationPatchOffsets`). One authored vertex
shader becomes many runtime variants, which is the whole expansion.

The 45 static pixel shaders not seen at runtime are simply ones the cached play
sessions never exercised, plus a few duplicate blobs.

**This validates the Unleashed strategy rather than threatening it.** Their
recompiler translates the *authored* shader and feeds vertex data through native
D3D12/Vulkan input layouts built from the vertex declaration, precisely so that
runtime fetch-patching never has to be reproduced. We therefore translate the
145 static containers, not 1254 permutations, and the precompiled shader cache
model holds.

`XMicrocodeBuilder` is a red herring: `XCreateMicrocodeBuilder` is reached only
from D3DX compiler internals (`CUAssembler::Assemble`, `Compiler::Emit`), which
are linked-in library code, not something Kameo's renderer drives.

Byte order: the `.xsh` blobs matched the XEX blobs with no swapping, so ucode is
stored in the same order in both.

## Constant buffer sizes

XenosRecomp assumes 256 `float4` vertex constants and 224 `float4` pixel
constants. Kameo's device stores both blocks at 256 registers
(`0x780` and `0x1780`, `0x1000` bytes apart — see `kameo_guest_device.h`), so
the pixel figure may need widening to 256 for this title. Confirm against what
the shaders actually reference before changing it.
