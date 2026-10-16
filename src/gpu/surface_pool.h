#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include <plume_render_interface.h>

#include "gpu/bindless.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

// Guest formats holding more than 8 bits a channel: the pond's wave simulation keeps its state in 2:10:10:10 and feeds it back every frame, which an 8-bit copy rounds into drifting bands.
inline bool WideGuestFormat(uint32_t format) { return format == 7 || format == 54; }
inline constexpr plume::RenderFormat kWideColorFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
// Surfaces resolved into a wide format, whose offscreen targets are created wide from then on.
extern std::set<uint32_t> g_wideSurfaces;
// Resolve destinations a draw has sampled with a wide fetch format; the destination texture's own header does not always say so.
extern std::set<uint32_t> g_wideResolveAddresses;

// One host target per guest surface an offscreen pass renders into, spanning every viewport drawn into it.
struct OffscreenTarget {
  uint32_t surface = 0;  // the guest D3DSurface this pass renders into
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  std::unique_ptr<plume::RenderTexture> color;
  std::unique_ptr<plume::RenderTextureView> color_view;
  std::unique_ptr<plume::RenderTexture> depth;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t cleared_frame = ~0ull;
};
extern std::unordered_map<uint64_t, OffscreenTarget> g_offscreen;
// A pass that outgrows its target replaces it while a command list in flight may still bind the old one.
extern DeferredDestroyQueue<OffscreenTarget> g_offscreenGraveyard;

// Depth-only passes (the shadow maps) have no colour target and no surface pointer, so they get depth-only framebuffers keyed on the viewport size.
struct DepthOnlyTarget {
  std::unique_ptr<plume::RenderTexture> depth;
  std::unique_ptr<plume::RenderTextureView> depth_view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t cleared_frame = ~0ull;
};
extern std::unordered_map<uint64_t, DepthOnlyTarget> g_depthOnly;

DepthOnlyTarget* EnsureDepthOnly(GraphicsSystem::DrawPathContext& ctx, uint32_t w, uint32_t h);
OffscreenTarget* EnsureOffscreen(GraphicsSystem::DrawPathContext& ctx, uint64_t key, uint32_t w, uint32_t h);

}  // namespace kameo::gpu::detail
