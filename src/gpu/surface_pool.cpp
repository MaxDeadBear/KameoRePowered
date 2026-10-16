#include "gpu/surface_pool.h"

#include <algorithm>

#include "gpu/log.h"

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
  target.depth = ctx.device->createTexture(
      plume::RenderTextureDesc::DepthTarget(w, h, plume::RenderFormat(ctx.depth_format)));
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
  target.framebuffer = ctx.device->createFramebuffer(desc);
  if (!target.framebuffer) {
    KGFX_ERROR("depth-only framebuffer {}x{} FAILED", w, h);
    return nullptr;
  }
  KGFX_INFO("depth-only render target {}x{} created", w, h);
  return &g_depthOnly.emplace(key, std::move(target)).first->second;
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
  target.color = ctx.device->createTexture(plume::RenderTextureDesc::ColorTarget(w, h, format));
  target.depth = ctx.device->createTexture(
      plume::RenderTextureDesc::DepthTarget(w, h, plume::RenderFormat(ctx.depth_format)));
  if (!target.color || !target.depth) {
    return nullptr;
  }
  target.color_view = target.color->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));

  const plume::RenderTexture* attachments[] = {target.color.get()};
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = attachments;
  desc.colorAttachmentsCount = 1;
  desc.depthAttachment = target.depth.get();
  target.framebuffer = ctx.device->createFramebuffer(desc);
  if (!target.framebuffer) {
    return nullptr;
  }
  KGFX_INFO("offscreen render target {}x{} created{}", w, h, format == kWideColorFormat ? " (16-bit float, resolved into a wide format)" : "");
  return &g_offscreen.emplace(key, std::move(target)).first->second;
}

}  // namespace kameo::gpu::detail
