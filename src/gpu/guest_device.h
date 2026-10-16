#pragma once

// Kameo's guest D3D device layout, read from the retail image.

#include <cstdint>

namespace kameo::gpu {

// D3D__pDevice: guest pointer to the single device instance. [V]
inline constexpr uint32_t kD3DDevicePtr = 0x82766ABC;

namespace dev {

// Command-buffer cursor: DrawIndexedVertices calls D3D::CDevice::KickOff when +0x0 would pass +0x8. [V]
inline constexpr uint32_t kCmdWritePtr = 0x000;
inline constexpr uint32_t kCmdEndPtr = 0x008;
// D3D::CDevice::StartNewSegment lays out a segment: +0x4 its hard end, +0x8 that less 160, and zeroes +0x36A0 and +0x36B0. [V]
inline constexpr uint32_t kCmdHardEndPtr = 0x004;
inline constexpr uint32_t kCmdSegmentStart = 0x36AC;
inline constexpr uint32_t kCmdSegmentPredication = 0x36A0;
// Vertex-data bytes BeginRingAlloc has taken in this segment; past its threshold it pulls the command limit down to force a KickOff. [V]
inline constexpr uint32_t kCmdSegmentDataBytes = 0x36B0;
// (PrimarySize >> 2) - 1, set by D3DDevice_SetRingBufferParameters and read by BlockOnPrimaryRange. [V]
inline constexpr uint32_t kPrimaryRingMask = 0x3690;
// D3DDevice_BeginVertices stores the write pointer the inlined EndVertices restores, then the vertex pointer and its dword count. [V]
inline constexpr uint32_t kBeginVerticesResume = 0x3534;
inline constexpr uint32_t kBeginVerticesData = 0x3538;
inline constexpr uint32_t kBeginVerticesDwords = 0x3540;
// The indexed form (drawTube's inlined BeginIndexedVertices) adds the index pointer and its dword count. [V]
inline constexpr uint32_t kBeginIndicesData = 0x353C;
inline constexpr uint32_t kBeginIndicesDwords = 0x3544;

// Guest pointer to the GPU read-back block, whose first word is the last fence the GPU finished (BlockOnFence, D3DQuery_GetData). [V]
inline constexpr uint32_t kGpuReadback = 0x2890;
// The CPU fence counter D3DQuery_Issue stamps into a query and BlockOnFence waits for. [V]
inline constexpr uint32_t kCpuFence = 0x289C;

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

// Base of one contiguous register and constant shadow, so [0x480, 0x2780) can be snapshotted as one block. [V]
inline constexpr uint32_t kRegisterShadowBase = 0x480;

// Xenos texture fetch constants: 6 dwords (24 bytes) per sampler. [V]
inline constexpr uint32_t kSamplerFetchConstants = 0x480;
inline constexpr uint32_t kSamplerFetchStride = 24;
inline constexpr uint32_t kSamplerFetchCount = 32;

// Shader float constants, 256 float4 per stage at device + 16 * StartRegister + 1920 (VS) or 6016 (PS). [V]
inline constexpr uint32_t kVertexShaderFloatConstants = 0x0780;
inline constexpr uint32_t kPixelShaderFloatConstants = 0x1780;
inline constexpr uint32_t kShaderFloatConstantCount = 256;

// Bool constants. Confirmed: x_k_ProcessLazyStreams_D3D flushes device+10112
// (0x2780) to GPU register 0x4900 = SHADER_CONSTANT_BOOL_000_031. [V]
inline constexpr uint32_t kVertexShaderBoolConstants = 0x2780;
inline constexpr uint32_t kPixelShaderBoolConstants = 0x2790;

// Shader program control, assembled by D3D::LazyWriteShaders.
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

// Vertex streams, 16 of them, written by SetStreamSource; the stride is a byte holding Stride >> 2. [V]
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
// D3DDevice_SetScissorRect (0x820C2408): the enable render state, and the packed PA_SC_WINDOW_SCISSOR_TL/BR it rebuilds as (y << 16) | x.
inline constexpr uint32_t kScissorTestEnable = 0x2EB0;
inline constexpr uint32_t kWindowScissorTL = 0x2D04;
inline constexpr uint32_t kWindowScissorBR = 0x2D08;

// Back-buffer surface, fetched when no render target is passed. [V]
inline constexpr uint32_t kBackBufferSurface = 0x367C;

}  // namespace dev

// Byte offsets into setRenderStateFunctions, the same enumeration as Sonic Unleashed's.

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

// Byte offsets into setSamplerStateFunctions; the WRAP and NONE defaults corroborate the order.

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


}  // namespace kameo::gpu
