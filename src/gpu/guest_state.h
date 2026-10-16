#pragma once

#include <array>
#include <rex/graphics/xenos.h>

// Guest GPU state decoding shared by the draw hooks and the draw path, free of plume so the hooks can include it; offsets come from Kameo's own D3D setters.

#include <cstdint>

namespace kameo::gpu {

// Xenos primitive values, not PC D3D9's: 5 is a fan and 13 a quad list.
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

// The device offsets the state snapshot reads every draw; the rest are in gpu/guest_device.h.
namespace state {

inline constexpr uint32_t kDepthControl = 0x2D74;      // RB_DEPTHCONTROL
inline constexpr uint32_t kBlendControl0 = 0x2D78;     // RB_BLENDCONTROL0
inline constexpr uint32_t kColorControl = 0x2D7C;      // RB_COLORCONTROL
inline constexpr uint32_t kModeControl = 0x2D88;       // PA_SU_SC_MODE_CNTL
inline constexpr uint32_t kAlphaRef = 0x2D44;          // float, already /255
inline constexpr uint32_t kColorMask = 0x2D1C;         // bits 0..3 = RGBA writes
inline constexpr uint32_t kPrimitiveResetIndex = 0x2D18;  // VGT_MULTI_PRIM_IB_RESET_INDX, SetRenderState_PrimitiveResetIndex 820C1908

// PA_SU_SC_MODE_CNTL multi_prim_ib_ena, written by SetRenderState_PrimitiveResetEnable 820C18D0 (insrwi 1 bit @10).
inline constexpr uint32_t kPrimitiveResetEnableBit = 1u << 21;

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


// States added later from the named setters, each from the setter's own insrwi field.

// D3DDevice_SetRenderState_HalfPixelOffset (0x820C1898):
//   lwz r10,0x2E00(r3); insrwi r10,r4,1,31; stw r10,0x2E00(r3)
// insrwi at BE bit 31 is the LSB, so this is bit 0 of the dword.
inline constexpr uint32_t kHalfPixelOffset = 0x2E00;
inline constexpr uint32_t kHalfPixelOffsetBit = 1u << 0;

inline std::array<float, 2> PixelCenterOffset(uint32_t pixel_center, float viewport_width, float viewport_height) {
  if ((pixel_center & kHalfPixelOffsetBit) != 0 || viewport_width <= 0.0f || viewport_height <= 0.0f) return {};
  return {1.0f / viewport_width, -1.0f / viewport_height};
}

// RB_DEPTHCONTROL's stencil half; the ref and masks are separate bytes, not fields of the register.
inline constexpr uint32_t kStencilEnableBit = 1u << 0;
inline constexpr uint32_t kStencilFuncShift = 8;
inline constexpr uint32_t kStencilFailShift = 11;
inline constexpr uint32_t kStencilPassShift = 14;
inline constexpr uint32_t kStencilZFailShift = 17;
inline constexpr uint32_t kStencilOpMask = 0x7;
inline constexpr uint32_t kTwoSidedStencilBit = 1u << 7;
inline constexpr uint32_t kStencilRef = 0x2D43;        // byte
inline constexpr uint32_t kStencilMask = 0x2D42;       // byte
inline constexpr uint32_t kStencilWriteMask = 0x2D41;  // byte

// RB_COLORCONTROL again: alpha-to-coverage.
//   AlphaToMaskEnable    0x820C1930  insrwi 1 bit @27 -> bit 4
inline constexpr uint32_t kAlphaToMaskEnableBit = 1u << 4;

// Point sprites, both plain dwords rather than packed fields.
//   PointSize            0x820C0D78  stfs 0x2ED4 (a FLOAT)
//   PointSpriteEnable    0x820C0D68  stw  0x2ECC
inline constexpr uint32_t kPointSize = 0x2ED4;
inline constexpr uint32_t kPointMinMax = 0x2DA8;
inline constexpr uint32_t kPointSpriteEnable = 0x2ECC;

// Polygon offset from SetRenderState_DepthBias and SlopeScaleDepthBias; the stored slope scale is the caller's value times 16.
inline constexpr uint32_t kPolyOffsetFrontScale = 0x2E90;   // float, x16
inline constexpr uint32_t kPolyOffsetFrontOffset = 0x2E94;  // float
inline constexpr uint32_t kPolyOffsetBackScale = 0x2E98;    // float, x16
inline constexpr uint32_t kPolyOffsetBackOffset = 0x2E9C;   // float
inline constexpr uint32_t kPolyOffsetFrontEnableBit = 1u << 11;  // in PA_SU_SC_MODE_CNTL
inline constexpr uint32_t kPolyOffsetBackEnableBit = 1u << 12;
inline constexpr float kPolyOffsetScaleUnit = 16.0f;

// D3DDevice_SetRenderState_SRGBWriteEnable (0x820C1208) broadcasts the same
// value to one dword per render target: 0x2F5C, 0x2F60, 0x2F64, 0x2F68.
inline constexpr uint32_t kSrgbWriteEnable0 = 0x2F5C;

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

// The 6-dword Xenos fetch constant SetTexture assembles at 1152 + 24*sampler, read directly because it is what the console's GPU sampled.

struct GuestTextureFetch {
  // Raw dwords because CaptureState fills them word by word; every field comes from the SDK's struct, never hand-written bit math.
  union {
    uint32_t words[6] = {};
    rex::graphics::xenos::xe_gpu_texture_fetch_t xe;
  };

  bool valid() const { return words[0] != 0 || words[1] != 0; }

  // What `dimension()` used to return: bits 0..1 are the fetch constant TYPE,
  // and kTexture is 2, so the old accessor answered "2" for every valid texture
  // and its "2 = 2D" comment looked confirmed for the wrong reason.
  rex::graphics::xenos::FetchConstantType fetch_type() const { return xe.type; }
  rex::graphics::xenos::DataDimension dimension() const { return xe.dimension; }

  uint32_t clamp_x() const { return uint32_t(xe.clamp_x); }
  uint32_t clamp_y() const { return uint32_t(xe.clamp_y); }
  uint32_t clamp_z() const { return uint32_t(xe.clamp_z); }
  uint32_t pitch() const { return xe.pitch * 32; }
  bool tiled() const { return xe.tiled != 0; }

  // kSigned is TWO'S COMPLEMENT, so its host format is SNORM. The 2*c-1 one is
  // kUnsignedBiased, which Kameo never uses.
  uint32_t sign(uint32_t channel) const { return (words[0] >> (2 + 2 * channel)) & 0x3; }
  uint32_t signs() const { return (words[0] >> 2) & 0xFF; }

  uint32_t format() const { return uint32_t(xe.format); }
  uint32_t endian() const { return uint32_t(xe.endianness); }
  bool stacked() const { return xe.stacked != 0; }
  // The base address field is in 4 KB pages, which is why texture data is page
  // aligned and why the +0x1000 bias below lands on a page boundary too.
  uint32_t address() const { return xe.base_address << 12; }

  // 2D only. The union also carries a 24-bit size_1d and an 11/11/10 size_3d,
  // so these are right only while dimension() is k2DOrStacked -- which is why
  // EnsureTexture censuses anything that is not.
  uint32_t width() const { return xe.size_2d.width + 1; }
  uint32_t height() const { return xe.size_2d.height + 1; }
  uint32_t depth() const { return xe.size_2d.stack_depth + 1; }

  // dword_3 bit 0: texel-count coordinates; not applied, so such fetches clamp or wrap to an edge texel.
  bool unnormalized_coords() const { return xe.num_format != 0; }

  uint32_t swizzle() const { return xe.swizzle; }
  // Signed 6-bit exponent bias scaling every texel by 2^exp_adjust; not applied, and censused so that stays visible.
  int32_t exp_adjust() const { return xe.exp_adjust; }
  uint32_t mag_filter() const { return uint32_t(xe.mag_filter); }
  uint32_t min_filter() const { return uint32_t(xe.min_filter); }
  uint32_t mip_filter() const { return uint32_t(xe.mip_filter); }
  uint32_t aniso_filter() const { return uint32_t(xe.aniso_filter); }
  // Signed 10-bit mip bias in 1/32 steps, applied as the host sampler's mipLODBias.
  int32_t lod_bias() const { return xe.lod_bias; }
  uint32_t border_color() const { return uint32_t(xe.border_color); }

  // Mip levels are in dword 4 and the mip address in dword 5; reading them the other way round silently disables the chain.
  uint32_t mip_min_level() const { return xe.mip_min_level; }
  uint32_t mip_max_level() const { return xe.mip_max_level; }
  bool packed_mips() const { return xe.packed_mips != 0; }
  uint32_t mip_address() const { return xe.mip_address << 12; }
};

// Xenos texture formats, the values that land in the fetch constant's low 6
// bits. Only the ones the renderer handles are named; the rest are logged by
// number when they turn up.
enum GuestTextureSign : uint32_t {
  kTextureSignUnsigned = 0,
  kTextureSignSigned = 1,
  kTextureSignUnsignedBiased = 2,
  kTextureSignGamma = 3,
};

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

// XGVERTEXFORMAT is a bitfield (bits 0..5 the Xenos vertex format, bits 8..11 the interpretation), so unseen types decode too.

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

// Must match XenosRecomp's USAGE_LOCATIONS, because plume's Vulkan backend uses the location verbatim.
inline uint32_t DeclUsageLocation(uint32_t usage, uint32_t usage_index) {
  switch (usage) {
    case kUsagePosition: return usage_index == 0 ? 0u : 15u;
    case kUsageNormal: return 1;
    case kUsageTangent: return 2;
    case kUsageBinormal: return 3;
    case kUsageColor: return usage_index == 0 ? 8u : 11u;
    case kUsageBlendIndices: return 9;
    case kUsageBlendWeight: return 10;
    case kUsageTexCoord:
      if (usage_index <= 3) return 4 + usage_index;
      if (usage_index <= 7) return 8 + usage_index;
      return 16;
    default: return 0;
  }
}

inline const char* DeclUsageSemantic(uint32_t usage) {
  static const char* kSemantics[kUsageCount] = {
      "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",  "TEXCOORD", "TANGENT",
      "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",    "DEPTH",    "SAMPLE"};
  return usage < kUsageCount ? kSemantics[usage] : "TEXCOORD";
}

// XenosRecomp declares these usages uint4 and the rest float4, so the usage, not the guest type, decides the host format's component type.
inline bool DeclUsageIsInteger(uint32_t usage) {
  return usage == kUsageBlendIndices || usage == kUsageNormal || usage == kUsageTangent ||
         usage == kUsageBinormal;
}

}  // namespace kameo::gpu
