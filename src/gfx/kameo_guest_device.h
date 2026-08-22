#pragma once

// Kameo guest D3D device layout.
//
// The Xbox 360 D3D library is statically linked into default.xex. Most state
// setters are INLINED at their call sites (only ~5 SetRenderState_* call sites
// are outlined), so state cannot be intercepted by hooking functions. Instead
// it lands in the device struct pointed to by D3D__pDevice, and the native
// renderer reads it there at draw time.
//
// PROVENANCE -- every offset below is derived from the retail vanilla image
// (assets/default.xex, code 820B0000-826DF9F8), NOT from an XDK header:
//
//   * D3D::InitializeApiState (0x820D8C88) decompiles with raw integer offsets
//     and walks both function-pointer tables, giving their base and extent
//     directly (loop bounds 0x184 and 0x50).
//   * The tables' initializer arrays in .data (0x82755130, 0x827555C0) hold
//     {aux, setter, default} triples per state. Resolving each `setter` against
//     the PDB-derived symbol names maps every index to a named D3D entry point.
//   * drawTextureQuadNoSetup (0x820DB8C8) reads the surface/viewport fields.
//
// Offsets are marked [V] verified (a named setter or an unambiguous decompiled
// access) or [I] inferred (position in a table whose neighbours are verified).
//
// NOTE: the IDB's stock `D3DDevice` type (size 0x2B00) is WRONG for this build
// -- it made Hex-Rays emit `pDevice[1].m_Constants...`, i.e. indexing past its
// own struct. It has been replaced with an opaque blob so D3D functions
// decompile with true offsets. Do not trust field names from that old type.
//
// TU builds relocate all of this; these constants are vanilla-only.

#include <cstdint>

namespace kameo::gfx {

// -- Globals ---------------------------------------------------------------

// D3D__pDevice: guest pointer to the single device instance. [V]
inline constexpr uint32_t kD3DDevicePtr = 0x82766ABC;

// -- Device field offsets --------------------------------------------------

namespace dev {

// Command-buffer cursor. DrawIndexedVertices compares +0x0 against +0x8 and
// calls D3D::CDevice::KickOff when it would overflow. (Hex-Rays shows these as
// HIDWORD(m_Mask[0]) / HIDWORD(m_Mask[1]); on big-endian PPC the high dword of
// a u64 at +0x0 IS +0x0.) [V]
inline constexpr uint32_t kCmdWritePtr = 0x000;
inline constexpr uint32_t kCmdEndPtr = 0x008;

// Dirty masks (u64 each). The constant setters OR a bit-range into these to
// mark which register blocks must be flushed to the command buffer at draw
// time. A native renderer can use them to skip unchanged uploads. [V]
inline constexpr uint32_t kDirtyVertexConstants = 0x010;  // SetVertexShaderConstantFN
inline constexpr uint32_t kDirtyPixelConstants = 0x018;   // SetPixelShaderConstantFN
inline constexpr uint32_t kDirtyTextures = 0x020;         // SetTexture

// setRenderStateFunctions[97]. Inlined SetRenderState calls dispatch through
// here: (*(void(**)(dev,u32))(dev + 0x60 + state))(dev, value), where `state`
// is a BYTE offset -- so index = state / 4. [V]
inline constexpr uint32_t kSetRenderStateFuncs = 0x060;
inline constexpr uint32_t kSetRenderStateFuncCount = 97;

// setSamplerStateFunctions[20]. Dispatched as
// (*(void(**)(dev,u32,u32))(dev + 0x1E4 + state))(dev, sampler, value). [V]
inline constexpr uint32_t kSetSamplerStateFuncs = 0x1E4;
inline constexpr uint32_t kSetSamplerStateFuncCount = 20;

// Parallel get-state tables, written by the same InitializeApiState loops
// (dev + 4*(i + 141) and dev + 4*(i + 238)). [V]
inline constexpr uint32_t kGetRenderStateFuncs = 0x234;
inline constexpr uint32_t kGetSamplerStateFuncs = 0x3B8;

// 0x480 is the base of a CONTIGUOUS Xenos register/constant shadow, not just
// the sampler block. Three independent confirmations:
//   * SetTexture writes fetch constants at `24 * (Sampler + 48) + device`.
//   * SetPixelShader replays a shader's constant table into `device + 1152`
//     (= 0x480) as {u16 dstOffset, u16 dwordCount} records, so shaders patch
//     arbitrary registers relative to this base.
//   * 32 fetch constants * 24 bytes = 0x300, ending exactly at 0x780 where the
//     vertex float constants begin -- the region is packed with no gap.
// A native renderer can therefore snapshot [0x480, 0x2780) as one block. [V]
inline constexpr uint32_t kRegisterShadowBase = 0x480;

// Xenos texture fetch constants: 6 dwords (24 bytes) per sampler. [V]
inline constexpr uint32_t kSamplerFetchConstants = 0x480;
inline constexpr uint32_t kSamplerFetchStride = 24;
inline constexpr uint32_t kSamplerFetchCount = 32;

// Shader float constants, float4 per register.
// SetVertexShaderConstantFN: device + 16 * StartRegister + 1920. [V]
// SetPixelShaderConstantFN:  device + 16 * StartRegister + 6016. [V]
// 256 registers each -> 0x1000 bytes, so the two blocks are contiguous.
inline constexpr uint32_t kVertexShaderFloatConstants = 0x0780;
inline constexpr uint32_t kPixelShaderFloatConstants = 0x1780;
inline constexpr uint32_t kShaderFloatConstantCount = 256;

// Bool constants. Confirmed: x_k_ProcessLazyStreams_D3D flushes device+10112
// (0x2780) to GPU register 0x4900 = SHADER_CONSTANT_BOOL_000_031. [V]
inline constexpr uint32_t kVertexShaderBoolConstants = 0x2780;
inline constexpr uint32_t kPixelShaderBoolConstants = 0x2790;

// Register-flush map, read off x_k_ProcessLazyStreams_D3D. Each entry is
// (device offset -> Xenos register base) and is what a native renderer would
// consult instead of parsing the command buffer. [V]
//   0x0780 -> 0x4000   vertex float constants
//   0x1780 -> 0x4400   pixel float constants
//   0x2780 -> 0x4900   boolean constants
//   0x2CC0 -> 0x2000
//   0x2D74 -> 0x2200
//   0x2DA4 -> 0x2280
//   0x2DF8 -> 0x2300
//   0x2E90 -> 0x2380
// Shader program control registers are assembled separately by
// D3D::LazyWriteShaders into device+11616 / +11620.
inline constexpr uint32_t kShaderControlRegs = 11616;  // 0x2D60

// Per-sampler byte caches read by SetTexture when clamping mip/aniso against
// the texture's own limits (device + Sampler + N). Exact meanings unconfirmed;
// recorded so the addresses aren't rediscovered later. [I]
inline constexpr uint32_t kSamplerByteCacheA = 12046;  // 0x2F0E
inline constexpr uint32_t kSamplerByteCacheB = 12072;  // 0x2F28
inline constexpr uint32_t kSamplerByteCacheC = 12098;  // 0x2F42

// Vertex declaration storage; InitVertexDeclarationFromFVF writes here. [V]
inline constexpr uint32_t kVertexDeclaration = 0x3010;

// Index buffer (D3DIndexBuffer*). SetIndices writes device + 12532. [V]
inline constexpr uint32_t kIndexBuffer = 0x30F4;

// Vertex streams, 16 of them. SetStreamSource writes:
//   offset  -> device + 0x310C + 8*stream
//   buffer  -> device + 0x3110 + 8*stream
//   stride  -> device + 0x3190 + stream, as a BYTE holding (Stride >> 2)
// The stride array ends exactly where the texture slots begin (0x31A0),
// which corroborates a 16-stream limit. [V]
inline constexpr uint32_t kStreamOffset = 0x310C;
inline constexpr uint32_t kStreamBuffer = 0x3110;
inline constexpr uint32_t kStreamStride = 0x3190;  // byte array, one per stream
inline constexpr uint32_t kStreamStrideShift = 2;  // stored value = bytes >> 2
inline constexpr uint32_t kStreamCount = 16;
inline constexpr uint32_t kDirtyStreams = 0x030;  // bit 0x400 set by SetStreamSource

// Current render target surface (index 0). [V]
inline constexpr uint32_t kRenderTarget0 = 0x30F8;
// Current depth/stencil surface. [V]
inline constexpr uint32_t kDepthStencil = 0x3108;

// Bound textures: 26 D3DBaseTexture* slots. InitializeApiState releases each
// across `while (v6 < 0x1A)` starting at dev + 12704. [V]
inline constexpr uint32_t kTextureSlots = 0x31A0;
inline constexpr uint32_t kTextureSlotCount = 26;

// _D3DVIEWPORT9: 6 consecutive dwords (X, Y, Width, Height, MinZ, MaxZ),
// copied out wholesale by drawTextureQuadNoSetup's save/restore. [V]
inline constexpr uint32_t kViewport = 0x3208;

// Back-buffer surface, fetched when no render target is passed. [V]
inline constexpr uint32_t kBackBufferSurface = 0x367C;

}  // namespace dev

// -- Render states ---------------------------------------------------------
//
// Values are BYTE offsets into the setRenderStateFunctions table (divide by 4
// for the array index). Verified against Kameo's own table: every entry below
// was confirmed either by a named setter at that index or by a default value
// that only makes sense for that state.
//
// This enumeration is IDENTICAL to Sonic Unleashed's -- both titles link the
// same XDK D3D revision -- so UnleashedRecomp's video.cpp is directly
// applicable here. Only the table BASE differs (Kameo 0x60, Unleashed 0x40),
// and Kameo has 97 states to Unleashed's 101.

enum GuestRenderState : uint32_t {
  // i=0..9 dispatch to a bare `blr` at 0x823BB178: legacy D3D9 states with no
  // Xenos equivalent. Their defaults are still applied, but the calls are
  // no-ops -- the native renderer can ignore them entirely. [V]

  D3DRS_ZENABLE = 40,               // i=10, default 1                    [V]
  D3DRS_ZFUNC = 44,                 // i=11, default 3 (LESSEQUAL)        [V]
  D3DRS_ZWRITEENABLE = 48,          // i=12, default 1                    [V]
  D3DRS_CULLMODE = 56,              // i=14, default 6 (CCW)              [V]
  D3DRS_ALPHABLENDENABLE = 60,      // i=15                               [V]
  D3DRS_SEPARATEALPHABLENDENABLE = 64,  // i=16                           [V]
  D3DRS_BLENDFACTOR = 68,           // i=17, default 0xFFFFFFFF           [V]
  D3DRS_SRCBLEND = 72,              // i=18, default 1 (ONE)              [V]
  D3DRS_DESTBLEND = 76,             // i=19, default 0 (ZERO)             [V]
  D3DRS_BLENDOP = 80,               // i=20, default 0 (ADD)              [V]
  D3DRS_SRCBLENDALPHA = 84,         // i=21, default 1 (ONE)              [I]
  D3DRS_DESTBLENDALPHA = 88,        // i=22, default 0 (ZERO)             [I]
  D3DRS_BLENDOPALPHA = 92,          // i=23, default 0 (ADD)              [I]
  D3DRS_ALPHATESTENABLE = 96,       // i=24, default 0                    [I]
  D3DRS_ALPHAREF = 100,             // i=25                               [V]
  D3DRS_ALPHAFUNC = 104,            // i=26, default 7 (ALWAYS)           [I]
  D3DRS_STENCILENABLE = 108,        // i=27                               [V]
  D3DRS_POINTSIZE = 176,            // i=44, default 1.0f                 [V]
  D3DRS_SCISSORTESTENABLE = 200,    // i=50 (SetScissorRect)              [V]
  D3DRS_SLOPESCALEDEPTHBIAS = 204,  // i=51                               [V]
  D3DRS_DEPTHBIAS = 208,            // i=52                               [V]
  D3DRS_COLORWRITEENABLE = 212,     // i=53, default 0xF                  [V]
  D3DRS_VIEWPORTENABLE = 404,       // i=77, default 1                    [V]
};

// -- Sampler states --------------------------------------------------------
//
// Byte offsets into setSamplerStateFunctions. Defaults corroborate the
// ordering: AddressU/V/W default to 0 (WRAP) and MipFilter to 2 (NONE).

enum GuestSamplerState : uint32_t {
  D3DSAMP_ADDRESSU = 0,           // i=0, default 0 (WRAP)                [I]
  D3DSAMP_ADDRESSV = 4,           // i=1, default 0 (WRAP)                [I]
  D3DSAMP_ADDRESSW = 8,           // i=2, default 0 (WRAP)                [I]
  D3DSAMP_BORDERCOLOR = 12,       // i=3                                  [V]
  D3DSAMP_MAGFILTER = 16,         // i=4                                  [V]
  D3DSAMP_MINFILTER = 20,         // i=5                                  [V]
  D3DSAMP_MIPFILTER = 24,         // i=6, default 2 (NONE)                [I]
  D3DSAMP_MIPMAPLODBIAS = 28,     // i=7                                  [V]
  D3DSAMP_MAXMIPLEVEL = 32,       // i=8                                  [V]
  D3DSAMP_MAXANISOTROPY = 36,     // i=9, default 1                       [V]
  D3DSAMP_MAGFILTERZ = 40,        // i=10                                 [V]
  D3DSAMP_MINFILTERZ = 44,        // i=11                                 [I]
  D3DSAMP_MIPFILTERZ = 48,        // i=12                                 [I]
  D3DSAMP_MINMIPLEVEL = 52,       // i=13, default 13                     [V]
  D3DSAMP_POINTBORDERENABLE = 76, // i=19, default 1                      [V]
};

// -- D3D entry points ------------------------------------------------------
//
// Outlined and named from the PDB: 73 functions, 1009 call sites. Recompiled
// bodies are emitted as weak aliases (DEFINE_REX_FUNC), so a strong definition
// in our own TU overrides them -- and __imp__sub_<addr> still reaches the
// original, which lets us WRAP (observe, then delegate) instead of replace
// while migrating incrementally.

namespace fn {

inline constexpr uint32_t kCreatePixelShader = 0x820C8360;
inline constexpr uint32_t kCreateVertexShader = 0x820C86D0;
inline constexpr uint32_t kCreateVertexDeclaration = 0x820C8A50;
inline constexpr uint32_t kCreateTexture = 0x820C7088;
inline constexpr uint32_t kCreateSurface = 0x820C7210;
inline constexpr uint32_t kCreateVertexBuffer = 0x820D33E8;

inline constexpr uint32_t kSetPixelShader = 0x820C84B8;
inline constexpr uint32_t kSetVertexShader = 0x820C8968;
inline constexpr uint32_t kSetVertexDeclaration = 0x820C89D0;
inline constexpr uint32_t kSetTexture = 0x820C76F0;
inline constexpr uint32_t kSetStreamSource = 0x820C26C0;
inline constexpr uint32_t kSetIndices = 0x820C2760;
inline constexpr uint32_t kSetRenderTarget = 0x820C27E0;
inline constexpr uint32_t kSetDepthStencilSurface = 0x820C2B78;
inline constexpr uint32_t kSetViewport = 0x820C24D0;
inline constexpr uint32_t kSetScissorRect = 0x820C2408;
inline constexpr uint32_t kSetVertexShaderConstantF = 0x820C7DC0;
inline constexpr uint32_t kSetPixelShaderConstantF = 0x820C7F08;

inline constexpr uint32_t kDrawVertices = 0x820CED48;
inline constexpr uint32_t kDrawVerticesUP = 0x820CE9D8;
inline constexpr uint32_t kDrawIndexedVertices = 0x820CEF88;
inline constexpr uint32_t kBeginVertices = 0x820CE738;

inline constexpr uint32_t kClear = 0x820C4E70;
inline constexpr uint32_t kClearF = 0x820C4F20;
inline constexpr uint32_t kResolve = 0x820C3460;
inline constexpr uint32_t kSwap = 0x820CF9D8;
inline constexpr uint32_t kPresent = 0x820D0048;

// A bare `blr`. Used by the D3D tables themselves for no-op states, and handy
// as a divert target when a hook must consume a call without side effects.
inline constexpr uint32_t kNoOpBlr = 0x823BB178;

}  // namespace fn

// -- Kameo's own render layer ----------------------------------------------
//
// Kameo renders the main scene through predicated EDRAM tiling, which Sonic
// Unleashed did not use. The native renderer must collapse this to a single
// full-resolution pass. These are the game's OWN outlined functions (not D3D
// internals), so they are ordinary REX_HOOK targets.

namespace tiling {

inline constexpr uint32_t kEdRamManagerInitialise = 0x820B1528;
inline constexpr uint32_t kEdRamManagerSetPhysicalScreen = 0x820B16F8;
inline constexpr uint32_t kEdRamManagerCreateRenderTarget = 0x820B12E0;
inline constexpr uint32_t kPredicatedSetupScenario = 0x820B1780;
inline constexpr uint32_t kPredicatedTilingBeginPass = 0x820B1998;
inline constexpr uint32_t kPredicatedTilingPresent = 0x820B1A90;
inline constexpr uint32_t kManualEndResolve = 0x820B1B10;

}  // namespace tiling

}  // namespace kameo::gfx
