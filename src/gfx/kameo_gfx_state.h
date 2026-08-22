#pragma once

// Guest GPU state decoding, shared between the capture side (the draw hooks in
// kameo_gfx_hooks.cpp) and the submission side (kameo_draw_path.cpp).
//
// Nothing here depends on plume, so the hooks can include it without pulling
// the renderer in.
//
// PROVENANCE -- the render-state offsets and bit positions below are read out
// of Kameo's OWN D3D setters, not guessed from a Xenos register reference. The
// setters are reached through the device's function table (see
// kameo_guest_device.h), so their disassembly is authoritative for this build:
//
//   D3DDevice_SetRenderState_ZEnable    820C05C8  insrwi 0x2D74, 1 bit @30 -> bit 1
//   ...              _ZWriteEnable      820C0618  insrwi 0x2D74, 1 bit @29 -> bit 2
//   ...              _ZFunc             820C0650  insrwi 0x2D74, 3 bits @25 -> bits 4..6
//   ...              _CullMode          820BFED8  insrwi 0x2D88, 3 bits @29 -> bits 0..2
//   ...              _AlphaTestEnable   820BFF48  insrwi 0x2D7C, 1 bit @28 -> bit 3
//   ...              _AlphaFunc         820C0470  insrwi 0x2D7C, 3 bits @29 -> bits 0..2
//   ...              _AlphaRef          820C0400  stfs   0x2D44 (Value/255, a FLOAT)
//   ...              _ColorWriteEnable  820C0C48  insrwi 0x2D1C, 4 bits @28 -> bits 0..3
//   ...              _SrcBlend          820C00C8  -> 0x2D78 bits 0..4
//   ...              _DestBlend         820C0160  -> 0x2D78 bits 8..12
//   ...              _BlendOp           820C0030  -> 0x2D78 bits 5..7
//
// 0x2D74/0x2D78/0x2D7C/0x2D88 are RB_DEPTHCONTROL / RB_BLENDCONTROL0 /
// RB_COLORCONTROL / PA_SU_SC_MODE_CNTL in the register shadow, which the
// flush map in kameo_guest_device.h independently places at 0x2200 + n.
//
// Note SetRenderState_AlphaBlendEnable(FALSE) writes 0x00010001 (ONE/ZERO, ADD)
// into RB_BLENDCONTROL0 itself, and SrcBlend/DestBlend/BlendOp return early
// while blending is off -- so the register alone describes the effective blend
// and there is no separate enable to track.

#include <cstdint>

namespace kameo::gfx {

// -- Primitive types -------------------------------------------------------
//
// These are the XENOS values, not PC D3D9's. The two agree up to 4, then
// diverge: 5 is a FAN here, not a strip. Two independent measurements say so --
// the UI emits its quads in perimeter order (TL,TR,BR,BL), which only closes
// correctly as a fan, and 13 = QUADLIST exists at all, which PC D3D9 has no
// value for. IDA's `D3DPT_TRIANGLESTRIP` label on 5 comes from a PC header and
// is wrong for this hardware.
enum GuestPrimitiveType : uint32_t {
  kPrimPointList = 1,
  kPrimLineList = 2,
  kPrimLineStrip = 3,
  kPrimTriangleList = 4,
  kPrimTriangleFan = 5,
  kPrimTriangleStrip = 6,
  kPrimRectangleList = 8,
  kPrimLineLoop = 12,
  kPrimQuadList = 13,
  kPrimQuadStrip = 14,
};

// -- Device offsets used by the draw path ----------------------------------
//
// Everything else lives in kameo_guest_device.h; these are the ones the state
// snapshot reads every draw.
namespace state {

inline constexpr uint32_t kDepthControl = 0x2D74;      // RB_DEPTHCONTROL
inline constexpr uint32_t kBlendControl0 = 0x2D78;     // RB_BLENDCONTROL0
inline constexpr uint32_t kColorControl = 0x2D7C;      // RB_COLORCONTROL
inline constexpr uint32_t kModeControl = 0x2D88;       // PA_SU_SC_MODE_CNTL
inline constexpr uint32_t kAlphaRef = 0x2D44;          // float, already /255
inline constexpr uint32_t kColorMask = 0x2D1C;         // bits 0..3 = RGBA writes

// RB_DEPTHCONTROL
inline constexpr uint32_t kDepthStencilEnableBit = 1u << 0;
inline constexpr uint32_t kDepthTestEnableBit = 1u << 1;
inline constexpr uint32_t kDepthWriteEnableBit = 1u << 2;
inline constexpr uint32_t kDepthFuncShift = 4;
inline constexpr uint32_t kDepthFuncMask = 0x7;

// RB_COLORCONTROL
inline constexpr uint32_t kAlphaFuncMask = 0x7;
inline constexpr uint32_t kAlphaTestEnableBit = 1u << 3;
// RB_COLORCONTROL's alpha function, the D3DCMPFUNC values minus one.
inline constexpr uint32_t kAlphaFuncNever = 0;
inline constexpr uint32_t kAlphaFuncLess = 1;
inline constexpr uint32_t kAlphaFuncEqual = 2;
inline constexpr uint32_t kAlphaFuncLEqual = 3;
inline constexpr uint32_t kAlphaFuncGreater = 4;
inline constexpr uint32_t kAlphaFuncNotEqual = 5;
inline constexpr uint32_t kAlphaFuncGEqual = 6;
inline constexpr uint32_t kAlphaFuncAlways = 7;

// PA_SU_SC_MODE_CNTL
inline constexpr uint32_t kCullFrontBit = 1u << 0;
inline constexpr uint32_t kCullBackBit = 1u << 1;
inline constexpr uint32_t kFrontFaceCwBit = 1u << 2;

}  // namespace state

// Comparison functions are the raw Xenos field, 0-based -- confirmed by the
// state table's ZFUNC default of 3 meaning LESSEQUAL.
enum GuestCompareFunc : uint32_t {
  kCompareNever = 0,
  kCompareLess = 1,
  kCompareEqual = 2,
  kCompareLessEqual = 3,
  kCompareGreater = 4,
  kCompareNotEqual = 5,
  kCompareGreaterEqual = 6,
  kCompareAlways = 7,
};

enum GuestBlendFactor : uint32_t {
  kBlendZero = 0,
  kBlendOne = 1,
  kBlendSrcColor = 4,
  kBlendInvSrcColor = 5,
  kBlendSrcAlpha = 6,
  kBlendInvSrcAlpha = 7,
  kBlendDestColor = 8,
  kBlendInvDestColor = 9,
  kBlendDestAlpha = 10,
  kBlendInvDestAlpha = 11,
  kBlendConstColor = 12,
  kBlendInvConstColor = 13,
  kBlendConstAlpha = 14,
  kBlendInvConstAlpha = 15,
  kBlendSrcAlphaSat = 16,
};

enum GuestBlendOp : uint32_t {
  kBlendOpAdd = 0,
  kBlendOpSrcMinusDest = 1,
  kBlendOpMin = 2,
  kBlendOpMax = 3,
  kBlendOpDestMinusSrc = 4,
};

// -- Texture fetch constants -----------------------------------------------
//
// D3DDevice_SetTexture assembles the real 6-dword Xenos fetch constant into the
// device shadow at 1152 + 24*sampler, out of the texture's Identifier,
// BaseFlush, MipFlush and Format words. That block is what the console's GPU
// sampled, so it is read directly rather than reassembled. Field positions
// verified against live captures on the title screen: a 2048x1688 k_8 atlas
// reads pitch=2048, tiled=1, width=2048, height=1688.

struct GuestTextureFetch {
  uint32_t words[6] = {};

  bool valid() const { return words[0] != 0 || words[1] != 0; }
  uint32_t dimension() const { return words[0] & 0x3; }        // 1 = 1D, 2 = 2D, 3 = 3D/cube
  uint32_t clamp_x() const { return (words[0] >> 10) & 0x7; }
  uint32_t clamp_y() const { return (words[0] >> 13) & 0x7; }
  uint32_t clamp_z() const { return (words[0] >> 16) & 0x7; }
  uint32_t pitch() const { return ((words[0] >> 22) & 0x1FF) * 32; }
  bool tiled() const { return (words[0] >> 31) != 0; }

  uint32_t format() const { return words[1] & 0x3F; }
  uint32_t endian() const { return (words[1] >> 6) & 0x3; }
  bool stacked() const { return ((words[1] >> 10) & 0x1) != 0; }
  // The base address field is in 4 KB pages, which is why texture data is page
  // aligned and why the +0x1000 bias below lands on a page boundary too.
  uint32_t address() const { return (words[1] >> 12) << 12; }

  uint32_t width() const { return (words[2] & 0x1FFF) + 1; }
  uint32_t height() const { return ((words[2] >> 13) & 0x1FFF) + 1; }
  uint32_t depth() const { return ((words[2] >> 26) & 0x3F) + 1; }

  uint32_t swizzle() const { return (words[3] >> 1) & 0xFFF; }
  uint32_t mag_filter() const { return (words[3] >> 19) & 0x3; }
  uint32_t min_filter() const { return (words[3] >> 21) & 0x3; }
  uint32_t mip_filter() const { return (words[3] >> 23) & 0x3; }
  uint32_t aniso_filter() const { return (words[3] >> 25) & 0x7; }
};

// Xenos texture formats, the values that land in the fetch constant's low 6
// bits. Only the ones the renderer handles are named; the rest are logged by
// number when they turn up.
enum GuestTextureFormat : uint32_t {
  kXenosFormat_1_REVERSE = 0,
  kXenosFormat_1 = 1,
  kXenosFormat_8 = 2,
  kXenosFormat_1_5_5_5 = 3,
  kXenosFormat_5_6_5 = 4,
  kXenosFormat_6_5_5 = 5,
  kXenosFormat_8_8_8_8 = 6,
  kXenosFormat_2_10_10_10 = 7,
  kXenosFormat_8_A = 8,
  kXenosFormat_8_B = 9,
  kXenosFormat_8_8 = 10,
  kXenosFormat_4_4_4_4 = 15,
  kXenosFormat_10_11_11 = 16,
  kXenosFormat_11_11_10 = 17,
  kXenosFormat_DXT1 = 18,
  kXenosFormat_DXT2_3 = 19,
  kXenosFormat_DXT4_5 = 20,
  kXenosFormat_24_8 = 22,
  kXenosFormat_24_8_FLOAT = 23,
  kXenosFormat_16 = 24,
  kXenosFormat_16_16 = 25,
  kXenosFormat_16_16_16_16 = 26,
  kXenosFormat_16_FLOAT = 30,
  kXenosFormat_16_16_FLOAT = 31,
  kXenosFormat_16_16_16_16_FLOAT = 32,
  kXenosFormat_32_FLOAT = 36,
  kXenosFormat_32_32_FLOAT = 37,
  kXenosFormat_32_32_32_32_FLOAT = 38,
  kXenosFormat_DXN = 49,
  kXenosFormat_8_8_8_8_AS_16_16_16_16 = 50,
  kXenosFormat_DXT1_AS_16_16_16_16 = 51,
  kXenosFormat_DXT2_3_AS_16_16_16_16 = 52,
  kXenosFormat_DXT4_5_AS_16_16_16_16 = 53,
  kXenosFormat_2_10_10_10_AS_16_16_16_16 = 54,
  kXenosFormat_DXT3A = 58,
  kXenosFormat_DXT5A = 59,
  kXenosFormat_CTX1 = 60,
};

// The page bias D3D itself applies when it turns a resource's stored base into
// a GPU fetch address:
//
//     ((((base >> 20) + 512) & 0x1000) + base)
//
// The first term contributes 0x1000 only when the address is >= 0xE0000000.
// This is NOT a fudge factor -- D3DDevice_SetTexture and
// D3DDevice_DrawIndexedVertices both compute it inline, and the Bink planes at
// 0xED70F000 needed exactly this to stop reading a page early.
inline uint32_t ApplyPageBias(uint32_t address) {
  return address + ((((address >> 20) + 512) & 0x1000));
}

// -- Vertex declaration element types --------------------------------------
//
// XGVERTEXFORMAT is a packed field, not an opaque enum, and it decodes cleanly:
//
//   bits 0..5   Xenos vertex data format (k_16_16, k_8_8_8_8, ...)
//   bits 8..11  interpretation: 0 UNORM, 1 SNORM, 2 UINT, 3 SINT/FLOAT,
//               8 D3DCOLOR (BGRA-ordered UNORM)
//
// Checked against every value Kameo emits and against the sizes implied by the
// measured element offsets: SHORT2 0x2C2359 -> fmt 25 (k_16_16),
// SHORT4 0x1A235A -> fmt 26, DEC3N 0x2A2187 -> fmt 7 (k_2_10_10_10),
// UBYTE4N 0x1A2086 -> fmt 6 (k_8_8_8_8). Decoding the bitfield rather than
// tabulating the enum means unseen types (FLOAT16_2, SHORT2N, ...) work too.

enum GuestVertexNumFormat : uint32_t {
  kVertexUNorm = 0,
  kVertexSNorm = 1,
  kVertexUInt = 2,
  kVertexSInt = 3,   // also FLOAT, distinguished by the data format
  kVertexColor = 8,  // D3DCOLOR: BGRA order
};

struct GuestVertexType {
  uint32_t data_format = 0;    // Xenos vertex format, bits 0..5
  uint32_t num_format = 0;     // GuestVertexNumFormat, bits 8..11
  uint32_t components = 0;     // 1..4
  uint32_t component_bits = 0; // 8, 16, 32, or 0 for packed-dword formats
  uint32_t size = 0;           // bytes in guest memory
  bool is_float = false;
  bool packed = false;         // whole dword is one packed value (DEC3N etc.)
};

inline GuestVertexType DecodeVertexType(uint32_t xg_format) {
  GuestVertexType t;
  t.data_format = xg_format & 0x3F;
  t.num_format = (xg_format >> 8) & 0xF;

  switch (t.data_format) {
    case 6:  // k_8_8_8_8  -- UBYTE4, UBYTE4N, D3DCOLOR
      t.components = 4;
      t.component_bits = 8;
      t.size = 4;
      break;
    case 7:   // k_2_10_10_10 -- DEC3N, UDEC3
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10
      t.components = 1;
      t.component_bits = 32;
      t.size = 4;
      t.packed = true;
      break;
    case 25:  // k_16_16 -- SHORT2, SHORT2N, USHORT2N
      t.components = 2;
      t.component_bits = 16;
      t.size = 4;
      break;
    case 26:  // k_16_16_16_16 -- SHORT4, SHORT4N, USHORT4N
      t.components = 4;
      t.component_bits = 16;
      t.size = 8;
      break;
    case 31:  // k_16_16_FLOAT -- FLOAT16_2
      t.components = 2;
      t.component_bits = 16;
      t.size = 4;
      t.is_float = true;
      break;
    case 32:  // k_16_16_16_16_FLOAT -- FLOAT16_4
      t.components = 4;
      t.component_bits = 16;
      t.size = 8;
      t.is_float = true;
      break;
    case 33:  // k_32
    case 36:  // k_32_FLOAT -- FLOAT1
      t.components = 1;
      t.component_bits = 32;
      t.size = 4;
      t.is_float = (t.data_format == 36);
      break;
    case 34:  // k_32_32
    case 37:  // k_32_32_FLOAT -- FLOAT2
      t.components = 2;
      t.component_bits = 32;
      t.size = 8;
      t.is_float = (t.data_format == 37);
      break;
    case 57:  // k_32_32_32_FLOAT -- FLOAT3
      t.components = 3;
      t.component_bits = 32;
      t.size = 12;
      t.is_float = true;
      break;
    case 35:  // k_32_32_32_32
    case 38:  // k_32_32_32_32_FLOAT -- FLOAT4
      t.components = 4;
      t.component_bits = 32;
      t.size = 16;
      t.is_float = (t.data_format == 38);
      break;
    default:
      break;  // size 0 -- caller reports it rather than reading garbage
  }
  return t;
}

// D3DDECLUSAGE, in the order XenosRecomp's USAGE_SEMANTICS uses.
enum GuestDeclUsage : uint32_t {
  kUsagePosition = 0,
  kUsageBlendWeight = 1,
  kUsageBlendIndices = 2,
  kUsageNormal = 3,
  kUsagePointSize = 4,
  kUsageTexCoord = 5,
  kUsageTangent = 6,
  kUsageBinormal = 7,
  kUsageTessFactor = 8,
  kUsagePositionT = 9,
  kUsageColor = 10,
  kUsageFog = 11,
  kUsageDepth = 12,
  kUsageSample = 13,
  kUsageCount = 14,
};

inline const char* DeclUsageSemantic(uint32_t usage) {
  static const char* kSemantics[kUsageCount] = {
      "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",  "TEXCOORD", "TANGENT",
      "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",    "DEPTH",    "SAMPLE"};
  return usage < kUsageCount ? kSemantics[usage] : "TEXCOORD";
}

// XenosRecomp declares BLENDINDICES, NORMAL, TANGENT and BINORMAL as `uint4`
// and everything else as `float4` (USAGE_TYPES in shader_recompiler.cpp). The
// input layout has to match that component type or pipeline creation fails, so
// this is what decides between a float and a uint host format -- not the guest
// declaration's own type.
inline bool DeclUsageIsInteger(uint32_t usage) {
  return usage == kUsageBlendIndices || usage == kUsageNormal || usage == kUsageTangent ||
         usage == kUsageBinormal;
}

}  // namespace kameo::gfx
