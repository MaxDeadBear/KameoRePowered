#include "gpu/surface_pool.h"

#include <algorithm>

#include "gpu/config.h"
#include "gpu/log.h"
#include "gpu/replay/replay_context.h"
#include "gpu/resource_check.h"

namespace kameo::gpu::detail {

std::set<uint32_t> g_wideSurfaces;
std::set<uint32_t> g_wideResolveAddresses;
std::unordered_map<uint64_t, OffscreenTarget> g_offscreen;
DeferredDestroyQueue<OffscreenTarget> g_offscreenGraveyard;
std::unordered_map<uint64_t, DepthOnlyTarget> g_depthOnly;

DepthOnlyTarget* EnsureDepthOnly(GraphicsSystem::DrawPathContext& ctx, uint32_t w, uint32_t h) {
  const uint64_t key = (uint64_t(w) << 32) | h;
  auto found = g_depthOnly.find(key);
  if (found != g_depthOnly.end()) {
    return &found->second;
  }
  DepthOnlyTarget target;
  target.width = w;
  target.height = h;
  target.depth = CreateTextureChecked(ctx.device,
      plume::RenderTextureDesc::DepthTarget(w, h, plume::RenderFormat(ctx.depth_format)), "depth-only target");
  if (!target.depth) {
    return nullptr;
  }
  target.depth_view = target.depth->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat(ctx.depth_format)));
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = nullptr;
  desc.colorAttachmentsCount = 0;
  desc.depthAttachment = target.depth.get();
  desc.depthAttachmentView = target.depth_view.get();
  target.framebuffer = CreateFramebufferChecked(ctx.device, desc, "depth-only framebuffer");
  if (!target.framebuffer) {
    KGFX_ERROR("depth-only framebuffer {}x{} FAILED", w, h);
    return nullptr;
  }
  KGFX_INFO("depth-only render target {}x{} created", w, h);
  return &g_depthOnly.emplace(key, std::move(target)).first->second;
}

// Each offscreen target is keyed by its guest surface's address, and the imposter system renders through scratch-pad surfaces it allocates afresh, so new keys keep arriving (about 15 a minute in a long session) and nothing reused the old ones.
// Every target holds a render-target and a depth descriptor out of plume's fixed 16384-entry D3D12 heaps; after a few hours the heaps ran out and the next framebuffer came back without descriptors, which crashed the first clear (issue #10).
// A target no pass has drawn into for this long is freed; one a pass needs again is simply recreated, and a resolve only reads a pass at most kameo_gfx_stale_offscreen_frames old.
void SweepIdleOffscreen(GraphicsSystem::DrawPathContext& ctx) {
  constexpr uint64_t kIdleFrames = 600;
  if (REXCVAR_GET(kameo_gfx_stale_offscreen_frames) < 0) return;  // unbounded reuse keeps every pass on purpose
  uint32_t freed = 0;
  for (auto it = g_offscreen.begin(); it != g_offscreen.end();) {
    if (it->second.cleared_frame != ~0ull && FramesSince(it->second.cleared_frame) > kIdleFrames) {
      g_offscreenGraveyard.queue(std::move(it->second), ctx.frame_index);
      it = g_offscreen.erase(it);
      ++freed;
    } else {
      ++it;
    }
  }
  static uint64_t freed_total = 0;
  freed_total += freed;
  if (freed) {
    static uint32_t reports = 0;
    if (reports++ < 20 || (reports % 50) == 0) KGFX_INFO("offscreen targets: freed {} idle ({} kept, {} freed this session)", freed, g_offscreen.size(), freed_total);
  }
}

OffscreenTarget* EnsureOffscreen(GraphicsSystem::DrawPathContext& ctx, uint64_t key, uint32_t w, uint32_t h) {
  const plume::RenderFormat format = g_wideSurfaces.count(uint32_t(key >> 32)) ? kWideColorFormat : plume::RenderFormat(ctx.color_format);
  auto found = g_offscreen.find(key);
  if (found != g_offscreen.end()) {
    // One target per surface spans every viewport drawn into it, so a later pass can need more room than the first.
    if (found->second.width >= w && found->second.height >= h && found->second.format == format) {
      return &found->second;
    }
    w = std::max(w, found->second.width);
    h = std::max(h, found->second.height);
    g_offscreenGraveyard.queue(std::move(found->second), ctx.frame_index);
    g_offscreen.erase(found);
  }
  OffscreenTarget target;
  target.surface = uint32_t(key >> 32);
  target.format = format;
  target.width = w;
  target.height = h;
  target.color = CreateTextureChecked(ctx.device, plume::RenderTextureDesc::ColorTarget(w, h, format), "offscreen colour");
  target.depth = CreateTextureChecked(ctx.device,
      plume::RenderTextureDesc::DepthTarget(w, h, plume::RenderFormat(ctx.depth_format)), "offscreen depth");
  if (!target.color || !target.depth) {
    return nullptr;
  }
  target.color_view = target.color->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));

  const plume::RenderTexture* attachments[] = {target.color.get()};
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = attachments;
  desc.colorAttachmentsCount = 1;
  desc.depthAttachment = target.depth.get();
  target.framebuffer = CreateFramebufferChecked(ctx.device, desc, "offscreen framebuffer");
  if (!target.framebuffer) {
    return nullptr;
  }
  KGFX_INFO("offscreen render target {}x{} created{}", w, h, format == kWideColorFormat ? " (16-bit float, resolved into a wide format)" : "");
  return &g_offscreen.emplace(key, std::move(target)).first->second;
}

}  // namespace kameo::gpu::detail
