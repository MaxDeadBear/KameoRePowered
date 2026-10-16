#include "gpu/sampler_cache.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

#include <plume_render_interface.h>
#include <plume_vulkan.h>

#include "gpu/config.h"
#include "gpu/log.h"
#include "gpu/textures/texture_heap.h"

namespace kameo::gpu::detail {
namespace {

namespace xe = rex::graphics::xenos;

struct CachedSampler {
  std::unique_ptr<plume::RenderSampler> sampler;
  uint32_t heap_index = 0;
};

std::unordered_map<uint64_t, CachedSampler> g_samplers;
struct SamplerMemo {
  uint64_t key = ~0ull;
  uint32_t heap_index = 0;
};
SamplerMemo g_samplerMemo[256];
uint32_t g_nextSamplerSlot = 0;

plume::RenderTextureAddressMode AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return plume::RenderTextureAddressMode::WRAP;
    case 1: return plume::RenderTextureAddressMode::MIRROR;
    case 2: return plume::RenderTextureAddressMode::CLAMP;
    case 3: return plume::RenderTextureAddressMode::MIRROR_ONCE;
    default: return plume::RenderTextureAddressMode::CLAMP;
  }
}

}  // namespace

uint32_t EnsureSampler(GraphicsSystem::DrawPathContext& ctx, const GuestTextureFetch& fetch, bool linear_depth) {
  // lod_bias and border_color live in words 4 and 5; without them every sampler differing only in those collapsed onto the first one created.
  const uint64_t key = (uint64_t(fetch.words[0] >> 10) & 0xFFF) |
                       (uint64_t(fetch.words[3] >> 19) << 12) |
                       (uint64_t(fetch.lod_bias() & 0x3FF) << 26) |
                       (uint64_t(fetch.border_color()) << 36) |
                       (uint64_t(std::clamp(REXCVAR_GET(kameo_gfx_anisotropy), 0, 16)) << 38) |
                       (uint64_t(linear_depth) << 43);
  // Most draws bind the same few samplers, so a direct-mapped memo answers before the map is searched.
  SamplerMemo& memo = g_samplerMemo[(key * 0x9E3779B97F4A7C15ull) >> 56];
  if (memo.key == key) return memo.heap_index;
  auto found = g_samplers.find(key);
  if (found != g_samplers.end()) {
    memo = {key, found->second.heap_index};
    return found->second.heap_index;
  }
  if (g_nextSamplerSlot >= kMaxBindlessSamplers) {
    return 0;
  }

  plume::RenderSamplerDesc desc;
  // Xenos filter fields: 0 point, 1 linear, 2 base map; anything not point is linear.
  desc.magFilter = fetch.mag_filter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  desc.minFilter = fetch.min_filter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  // Xenos mip_filter: 0 point, 1 linear, 2 basemap (no mip).
  desc.mipmapMode = fetch.mip_filter() == 1 ? plume::RenderMipmapMode::LINEAR
                                            : plume::RenderMipmapMode::NEAREST;
  desc.addressU = AddressMode(fetch.clamp_x());
  desc.addressV = AddressMode(fetch.clamp_y());
  desc.addressW = AddressMode(fetch.clamp_z());

  // A2XX_SQ_TEX_4_LOD_BIAS carries 5 fractional bits; Kameo's are all negative (-1.0, -1.5, -2.0 levels).
  desc.mipLODBias = float(fetch.lod_bias()) / 32.0f;

  // k_ABGR_White is the only non-black border Kameo uses.
  desc.borderColor = fetch.border_color() == uint32_t(xe::BorderColor::k_ABGR_White)
                         ? plume::RenderBorderColor::OPAQUE_WHITE
                         : plume::RenderBorderColor::TRANSPARENT_BLACK;

  // AnisoFilter is a max ratio (kMax_2_1 = 2 means 2x); only over an all-LINEAR sampler, since D3D12's FILTER_ANISOTROPIC forces linear min/mag where Vulkan does not.
  const uint32_t aniso = fetch.aniso_filter();
  const bool aniso_ok = aniso >= uint32_t(xe::AnisoFilter::kMax_2_1) &&
                        aniso <= uint32_t(xe::AnisoFilter::kMax_16_1) &&
                        desc.minFilter == plume::RenderFilter::LINEAR &&
                        desc.magFilter == plume::RenderFilter::LINEAR &&
                        desc.mipmapMode == plume::RenderMipmapMode::LINEAR;
  desc.anisotropyEnabled = aniso_ok;
  desc.maxAnisotropy = aniso_ok ? (1u << (aniso - 1)) : 1u;
  // The shadow maps are read with point filtering, so every map texel lands as a hard square; interpolating the depth before the shader's compare moves the edge smoothly between texels.
  if (linear_depth) {
    desc.minFilter = plume::RenderFilter::LINEAR;
    desc.magFilter = plume::RenderFilter::LINEAR;
    desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
    desc.anisotropyEnabled = false;
    desc.maxAnisotropy = 1;
  }
  const uint32_t forced_aniso = uint32_t(std::clamp(REXCVAR_GET(kameo_gfx_anisotropy), 0, 16));
  // Forcing it also makes the mip filter trilinear, since D3D12's anisotropic filter always is.
  if (forced_aniso > 1 && desc.minFilter == plume::RenderFilter::LINEAR && desc.magFilter == plume::RenderFilter::LINEAR) {
    desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = std::max(desc.maxAnisotropy, forced_aniso);
  }

  CachedSampler entry;
  entry.sampler = ctx.device->createSampler(desc);
  if (!entry.sampler) {
    return 0;
  }
  entry.heap_index = g_nextSamplerSlot++;
  ctx.sampler_set->setSampler(entry.heap_index, entry.sampler.get());
  const uint32_t index = entry.heap_index;
  g_samplers.emplace(key, std::move(entry));
  return index;
}

bool ShadowMapsFilterable(const GraphicsSystem::DrawPathContext& ctx) {
  if (!ctx.spirv) return true;
  static const bool filterable = [&] {
    VkFormatProperties props = {};
    vkGetPhysicalDeviceFormatProperties(static_cast<plume::VulkanDevice*>(ctx.device)->physicalDevice, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
    const bool linear = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    KGFX_INFO("vulkan shadow maps {} filterable", linear ? "are" : "are NOT");
    return linear;
  }();
  return filterable;
}

void ReleaseSamplers() {
  g_samplers.clear();
  std::fill(std::begin(g_samplerMemo), std::end(g_samplerMemo), SamplerMemo{});
  g_nextSamplerSlot = 0;
}

}  // namespace kameo::gpu::detail
