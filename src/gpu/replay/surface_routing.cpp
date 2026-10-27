#include <algorithm>
#include <cmath>
#include <iterator>

#include "gpu/draw/draw_constants.h"
#include "gpu/replay/replay_context.h"
#include "gpu/replay/screen_layout.h"
#include "gpu/surface_pool.h"

namespace kameo::gpu::detail {

void PlanSurfaces(ReplayContext& rc) {
  const DrawFrame& frame = rc.frame;
  uint32_t& main_surface = rc.main_surface;
  auto& surface_dims = rc.surface_dims;
  auto& surface_extent = rc.surface_extent;
  for (const DrawCall& call : frame.draws) {
    if (call.kind != CommandKind::Draw) {
      continue;
    }
    if (uint32_t(call.viewport[2]) == kGuestWidth && uint32_t(call.viewport[3]) == kGuestHeight) {
      main_surface = call.rt_surface;
      break;
    }
  }
  // An offscreen surface's size comes from the resolve that consumes it.
  for (const DrawCall& call : frame.draws) {
    if (call.kind != CommandKind::Resolve || call.resolve_depth) {
      continue;
    }
    if (call.rt_surface == 0 || call.rt_surface == main_surface) {
      continue;
    }
    surface_dims[call.rt_surface] = {call.resolve_width, call.resolve_height};
    if (WideGuestFormat(call.resolve_format)) g_wideSurfaces.insert(call.rt_surface);
  }
  // One guest target holds several passes at different viewports, so the host target has to span all of them.
  for (const DrawCall& call : frame.draws) {
    if (call.kind != CommandKind::Draw) {
      continue;
    }
    const float ex = call.viewport[0] + call.viewport[2];
    const float ey = call.viewport[1] + call.viewport[3];
    auto& e = surface_extent[call.rt_surface];
    e.first = std::max(e.first, ex > 0.0f ? uint32_t(ex) : 0u);
    e.second = std::max(e.second, ey > 0.0f ? uint32_t(ey) : 0u);
  }
}

bool RouteDrawTarget(ReplayContext& rc, const DrawCall& call, uint32_t& off_w, uint32_t& off_h) {
  GraphicsSystem::DrawPathContext& ctx = rc.ctx;
  plume::RenderCommandList* list = rc.list;
  plume::RenderTexture* const target = rc.target;
  const plume::RenderFramebuffer* const framebuffer = rc.framebuffer;
  const uint32_t main_surface = rc.main_surface;
  const auto& surface_dims = rc.surface_dims;
  const auto& surface_extent = rc.surface_extent;
  plume::RenderTexture*& current_texture = rc.current_texture;
  const plume::RenderFramebuffer*& current_framebuffer = rc.current_framebuffer;
  plume::RenderFormat& current_color_format = rc.current_color_format;
  uint64_t& current_key = rc.current_key;
  OcclusionReplay& occlusion = rc.occlusion;
  // A viewport smaller than the guest's frame means an offscreen pass; anything full-size is the scene itself.
  const uint32_t vp_w = uint32_t(call.viewport[2]);
  const uint32_t vp_h = uint32_t(call.viewport[3]);
  bool offscreen = vp_w != 0 && vp_h != 0 && (vp_w != kGuestWidth || vp_h != kGuestHeight);
  // ...but never for a draw aimed at the scene's own surface, which is the guest drawing into part of the frame.
  if (offscreen && main_surface != 0 && call.rt_surface == main_surface) {
    offscreen = false;
  }
  off_w = vp_w;
  off_h = vp_h;
  // A surface that is not the scene's is an offscreen pass even when the guest left the viewport at full size.
  if (!offscreen && main_surface != 0 && call.rt_surface != main_surface) {
    auto dims = surface_dims.find(call.rt_surface);
    if (dims != surface_dims.end()) {
      offscreen = true;
      off_w = dims->second.first;
      off_h = dims->second.second;
    }
  }
  // One host target per guest surface, spanning every viewport drawn into it: drawOrthoObj draws the book's portraits at the caller's viewport into the page art's target, and the imposters draw at 64, 128 and 256 into one surface.
  if (offscreen && call.rt_surface != 0) {
    auto extent = surface_extent.find(call.rt_surface);
    if (extent != surface_extent.end()) {
      off_w = std::max(off_w, extent->second.first);
      off_h = std::max(off_h, extent->second.second);
    }
  }
  const uint64_t want_key = !offscreen ? 0 : call.rt_surface != 0 ? (uint64_t(call.rt_surface) << 32) : ((uint64_t(off_w) << 16) ^ off_h);

  // A depth-only pass goes to a depth-only framebuffer whatever the colour routing decided: its PSO has no render targets.
  if (call.depth_only) {
    DepthOnlyTarget* d = EnsureDepthOnly(ctx, vp_w ? vp_w : kGuestWidth,
                                         vp_h ? vp_h : kGuestHeight);
    if (d == nullptr) {
      return false;
    }
    if (current_framebuffer != d->framebuffer.get()) {
      SuspendVulkanQuery(ctx, list, occlusion);
      // ClearDepthStencilView is rejected on a resource left in COMMON.
      list->barriers(plume::RenderBarrierStage::GRAPHICS,
                     plume::RenderTextureBarrier(d->depth.get(),
                                                 plume::RenderTextureLayout::DEPTH_WRITE));
      current_texture = nullptr;
      current_framebuffer = d->framebuffer.get();
      current_key = ~0ull;  // never matches a colour key, so the next colour draw re-establishes its own framebuffer
      list->setFramebuffer(current_framebuffer);
      RebindSceneLayout(rc);
    }
    if (d->cleared_frame != SceneStamp()) {
      d->cleared_frame = SceneStamp();
      list->clearDepthStencil(true, true, 1.0f, 0);
    }
  } else if (want_key != current_key) {
    SuspendVulkanQuery(ctx, list, occlusion);
    if (want_key == 0) {
      current_texture = target;
      current_framebuffer = framebuffer;
      current_color_format = plume::RenderFormat(ctx.color_format);
    } else if (OffscreenTarget* off = EnsureOffscreen(ctx, want_key, off_w, off_h)) {
      // The depth attachment needs its own transition: clearing it in COMMON is rejected at submit.
      const plume::RenderTextureBarrier to_draw[] = {
          plume::RenderTextureBarrier(off->color.get(), plume::RenderTextureLayout::COLOR_WRITE),
          plume::RenderTextureBarrier(off->depth.get(), plume::RenderTextureLayout::DEPTH_WRITE),
      };
      list->barriers(plume::RenderBarrierStage::GRAPHICS, to_draw, uint32_t(std::size(to_draw)));
      current_texture = off->color.get();
      current_framebuffer = off->framebuffer.get();
      current_color_format = off->format;
      list->setFramebuffer(current_framebuffer);
      // Each offscreen target is cleared once per frame, the first time the stream lands on it.
      if (off->cleared_frame != SceneStamp()) {
        off->cleared_frame = SceneStamp();
        list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        list->clearDepthStencil(true, true, 1.0f, 0);
      }
    } else {
      current_texture = target;
      current_framebuffer = framebuffer;
      current_color_format = plume::RenderFormat(ctx.color_format);
    }
    if (want_key == 0) {
      list->setFramebuffer(current_framebuffer);
    }
    current_key = want_key;
    RebindSceneLayout(rc);
  }
  return true;
}

void ReplayDepthClear(ReplayContext& rc, const DrawCall& call) {
  plume::RenderCommandList* list = rc.list;
  const plume::RenderTexture* current_texture = rc.current_texture;
  const uint32_t main_surface = rc.main_surface;
  const bool on_screen = current_texture == rc.target;
  const GuestToHost to_host = on_screen ? GuestToHostTransform(rc.width, rc.height) : GuestToHost{};
  const float scale_x = to_host.scale_x;
  const float scale_y = to_host.scale_y;
  const float origin_x = to_host.origin_x;
  const float origin_y = to_host.origin_y;
  const plume::RenderRect rect(int32_t(std::floor(origin_x + call.clear_rect[0] * scale_x)), int32_t(std::floor(origin_y + call.clear_rect[1] * scale_y)), int32_t(std::ceil(origin_x + call.clear_rect[2] * scale_x)), int32_t(std::ceil(origin_y + call.clear_rect[3] * scale_y)));
  // An unrouted surface falls back to the scene target, where a colour clear would wipe the scene.
  if (call.clear_colour && current_texture != nullptr && (!on_screen || call.rt_surface == main_surface)) {
    const float* c = call.clear_colour_value;
    list->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]), &rect, 1);
  }
  if (call.clear_depth_stencil) list->clearDepthStencil(true, true, call.clear_depth, 0, &rect, 1);
}

}  // namespace kameo::gpu::detail
