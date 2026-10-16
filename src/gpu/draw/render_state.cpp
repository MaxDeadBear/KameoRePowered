#include "gpu/draw/render_state.h"

#include <cstring>
#include <string>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <fmt/format.h>

#include "gpu/draw/capture.h"
#include "gpu/draw_path.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {
namespace {

// Thresholds for clip(alpha - threshold): below any possible alpha, and above any possible alpha.
constexpr float kAlphaPassAll = -1.0f;
constexpr float kAlphaPassNone = 2.0f;

}  // namespace

void CaptureState(uint8_t* base, uint32_t device, DrawCall* call, bool tally) {
  call->depth_control = Load32(base, device + state::kDepthControl);
  call->blend_control = Load32(base, device + state::kBlendControl0);
  call->color_control = Load32(base, device + state::kColorControl);
  call->mode_control = Load32(base, device + state::kModeControl);
  const uint32_t color_mask_raw = Load32(base, device + state::kColorMask);
  call->color_mask = color_mask_raw & 0xF;
  call->guest_pass = g_guestPass;
  call->hud_group = g_hudGroup;
  call->half_pixel = Load32(base, device + state::kHalfPixelOffset) & state::kHalfPixelOffsetBit;
  call->stencil_enable = call->depth_control & state::kStencilEnableBit;
  // Written either way, so a reused probe never carries a stale value into the comparison.
  call->stencil_ref_mask = call->stencil_enable ? (Load8(base, device + state::kStencilRef) | (Load8(base, device + state::kStencilMask) << 8) | (Load8(base, device + state::kStencilWriteMask) << 16)) : 0;
  call->alpha_to_mask = call->color_control & state::kAlphaToMaskEnableBit;
  call->point_sprite = Load32(base, device + state::kPointSpriteEnable);
  call->point_minmax = call->point_sprite ? Load32(base, device + state::kPointMinMax) : 0;
  call->srgb_write = Load32(base, device + state::kSrgbWriteEnable0);
  // Front and back polygon offsets differ but the host has one pair; take the front, which is what decals use.
  if (call->mode_control & state::kPolyOffsetFrontEnableBit) {
    call->poly_offset = LoadFloat(base, device + state::kPolyOffsetFrontOffset);
    call->poly_offset_scale =
        LoadFloat(base, device + state::kPolyOffsetFrontScale) / state::kPolyOffsetScaleUnit;
  } else {
    call->poly_offset = 0.0f;
    call->poly_offset_scale = 0.0f;
  }
  call->rt_surface = Load32(base, device + dev::kRenderTarget0);
  // The translated shaders only clip alpha < threshold (GEQUAL), so the guest's alpha func is folded into the threshold; GREATER nudges it up by 1/512, between zero and the smallest 8-bit alpha.
  const uint32_t alpha_func = call->color_control & state::kAlphaFuncMask;
  const bool alpha_test = (call->color_control & state::kAlphaTestEnableBit) != 0;
  const float ref = alpha_test ? LoadFloat(base, device + state::kAlphaRef) : 0.0f;
  if (!alpha_test) {
    call->alpha_ref = kAlphaPassAll;
  } else {
    switch (alpha_func) {
      case state::kAlphaFuncNever:   call->alpha_ref = kAlphaPassNone; break;
      case state::kAlphaFuncGreater: call->alpha_ref = ref + (1.0f / 512.0f); break;
      case state::kAlphaFuncGEqual:  call->alpha_ref = ref; break;
      case state::kAlphaFuncAlways:  call->alpha_ref = kAlphaPassAll; break;
      default: {
        // LESS / LEQUAL / EQUAL / NOTEQUAL cannot be one one-sided clip, so they pass everything; a draw with a halo would point here.
        static uint32_t unsupported_alpha_func[8] = {};
        static uint64_t last_report = 0;
        ++unsupported_alpha_func[alpha_func & 7];
        if (g_captureFrame - last_report > 600) {
          last_report = g_captureFrame;
          std::string tally;
          for (uint32_t i = 0; i < 8; ++i) {
            if (unsupported_alpha_func[i]) tally += fmt::format("func{}x{} ", i, unsupported_alpha_func[i]);
          }
          KGFX_WARN("ALPHA TEST comparisons this renderer cannot express, so those draws are NOT "
                      "clipped and render solid: {}(ref {:.4f})", tally, ref);
        }
        call->alpha_ref = kAlphaPassAll;
        break;
      }
    }
  }

  for (uint32_t i = 0; i < 4; ++i) {
    call->viewport[i] = float(Load32(base, device + dev::kViewport + 4 * i));
  }
  call->viewport[4] = LoadFloat(base, device + dev::kViewport + 16);
  call->viewport[5] = LoadFloat(base, device + dev::kViewport + 20);
  call->scissor_enable = Load32(base, device + dev::kScissorTestEnable) != 0;
  if (call->scissor_enable) {
    const uint32_t tl = Load32(base, device + dev::kWindowScissorTL);
    const uint32_t br = Load32(base, device + dev::kWindowScissorBR);
    call->scissor[0] = int32_t(tl & 0x7FFF);
    call->scissor[1] = int32_t((tl >> 16) & 0x7FFF);
    call->scissor[2] = int32_t(br & 0x7FFF);
    call->scissor[3] = int32_t((br >> 16) & 0x7FFF);
  } else {
    std::memset(call->scissor, 0, sizeof(call->scissor));
  }

  // The fetch constants and the bound-texture slots are each contiguous in the shadow, and this runs on every draw and every merge probe, so they are swapped a vector at a time rather than 112 scalar loads.
  static_assert(sizeof(GuestTextureFetch) == dev::kSamplerFetchStride);
  static_assert((kSamplerCount * dev::kSamplerFetchStride) % 32 == 0);
#if defined(__AVX2__)
  {
    const __m256i swap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const uint8_t* fetch = GuestData(base, device + dev::kSamplerFetchConstants);
    auto* fetch_dst = reinterpret_cast<uint8_t*>(call->textures);
    for (uint32_t i = 0; i < kSamplerCount * dev::kSamplerFetchStride; i += 32) {
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(fetch_dst + i), _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(fetch + i)), swap));
    }
    const uint8_t* objects = GuestData(base, device + dev::kTextureSlots);
    auto* object_dst = reinterpret_cast<uint8_t*>(call->texture_objects);
    for (uint32_t i = 0; i < kSamplerCount * 4; i += 32) {
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(object_dst + i), _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(objects + i)), swap));
    }
  }
#else
  for (uint32_t s = 0; s < kSamplerCount; ++s) {
    const uint32_t fetch = device + dev::kSamplerFetchConstants + dev::kSamplerFetchStride * s;
    for (uint32_t w = 0; w < 6; ++w) {
      call->textures[s].words[w] = Load32(base, fetch + 4 * w);
    }
    call->texture_objects[s] = Load32(base, device + dev::kTextureSlots + 4 * s);
  }
#endif

  // Bool constants: the vertex block in bits 0..15, the pixel block in 16..31, the packing XenosRecomp's b{N} defines assume.
  const uint32_t vs_bools = Load32(base, device + dev::kVertexShaderBoolConstants);
  const uint32_t ps_bools = Load32(base, device + dev::kPixelShaderBoolConstants);
  call->booleans = (vs_bools & 0xFFFF) | ((ps_bools & 0xFFFF) << 16);
}

uint64_t StreamIdentity(uint8_t* base, uint32_t device, const HostLayout& layout) {
  uint64_t key = 1469598103934665603ull;
  uint32_t seen = 0;
  for (const HostElement& e : layout.elements) {
    if (e.stream >= 16 || (seen & (1u << e.stream)) != 0) continue;
    seen |= 1u << e.stream;
    const uint32_t buffer = Load32(base, device + dev::kStreamBuffer + 8 * e.stream);
    const uint32_t fetch0 = buffer ? Load32(base, buffer + 12) : 0;
    const uint32_t fetch1 = buffer ? Load32(base, buffer + 16) : 0;
    const uint32_t byte_offset = Load32(base, device + dev::kStreamOffset + 8 * e.stream);
    const uint32_t stride = Load8(base, device + dev::kStreamStride + e.stream);
    for (uint64_t v : {uint64_t(e.stream), uint64_t(buffer), uint64_t(fetch0), uint64_t(fetch1), uint64_t(byte_offset), uint64_t(stride)}) {
      key = (key ^ v) * 1099511628211ull;
    }
  }
  return key;
}

bool SameCapturedState(const DrawCall& a, const DrawCall& b) {
  return a.depth_control == b.depth_control && a.blend_control == b.blend_control && a.color_control == b.color_control && a.mode_control == b.mode_control &&
         a.color_mask == b.color_mask && a.guest_pass == b.guest_pass && a.half_pixel == b.half_pixel && a.stencil_enable == b.stencil_enable &&
         a.stencil_ref_mask == b.stencil_ref_mask && a.alpha_to_mask == b.alpha_to_mask && a.point_sprite == b.point_sprite && a.point_minmax == b.point_minmax &&
         a.srgb_write == b.srgb_write && a.poly_offset == b.poly_offset && a.poly_offset_scale == b.poly_offset_scale && a.rt_surface == b.rt_surface &&
         a.alpha_ref == b.alpha_ref && a.booleans == b.booleans && std::memcmp(a.viewport, b.viewport, sizeof(a.viewport)) == 0 && a.scissor_enable == b.scissor_enable && std::memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0 &&
         std::memcmp(a.textures, b.textures, sizeof(a.textures)) == 0 && std::memcmp(a.texture_objects, b.texture_objects, sizeof(a.texture_objects)) == 0;
}

}  // namespace kameo::gpu::detail
