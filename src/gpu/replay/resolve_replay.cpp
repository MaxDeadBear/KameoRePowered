#include <algorithm>
#include <cmath>
#include <set>

#include "gpu/config.h"
#include "gpu/log.h"
#include "gpu/replay/replay_context.h"
#include "gpu/replay/screen_layout.h"
#include "gpu/resolve.h"
#include "gpu/scene_resolve_state.h"
#include "gpu/surface_pool.h"

namespace kameo::gpu::detail {
namespace {

// The offscreen pass a resolve of a non-scene surface reads, or null when none qualifies.
OffscreenTarget* FindOffscreenSource(const DrawCall& call) {
  // Matched by surface alone, not surface and resolve size: imposterModelRender draws icons at the scratch pad's size and resolves them into differently sized textures.
  // The most recent pass answers when this frame has none, since a render target keeps its contents: the storybook renders each portrait once and resolves it while the book is open.
  OffscreenTarget* off_match = nullptr;
  OffscreenTarget* off_stale = nullptr;
  for (auto& [k, t] : g_offscreen) {
    if (t.surface != call.rt_surface) {
      continue;
    }
    if (t.cleared_frame == SceneStamp()) {
      off_match = &t;
      break;
    }
    if (t.cleared_frame != ~0ull &&
        (off_stale == nullptr || t.cleared_frame > off_stale->cleared_frame)) {
      off_stale = &t;
    }
  }
  // A pass from an earlier screen is no longer this surface's content: the console's one eDRAM would have been overwritten (the warrior screen's card frames drawn over the main menu).
  const int32_t stale_max = REXCVAR_GET(kameo_gfx_stale_offscreen_frames);
  if (off_match == nullptr && off_stale != nullptr &&
      (stale_max < 0 ||
       FramesSince(off_stale->cleared_frame) <= uint64_t(stale_max))) {
    off_match = off_stale;
  } else if (off_match == nullptr && off_stale != nullptr) {
    static std::set<uint32_t> reported_stale;
    if (reported_stale.size() < 16 && reported_stale.insert(call.resolve_address).second) {
      KGFX_INFO("resolve {:08X} {}x{}: surface {:08X} last rendered {} frames ago; too old "
                  "to answer this resolve",
                  call.resolve_address, call.resolve_width, call.resolve_height,
                  call.rt_surface, FramesSince(off_stale->cleared_frame));
    }
  }
  // Last resort, a pass of exactly the resolve's size: the storybook portraits render through one surface object and resolve from another (matching by eDRAM base instead was measured to break the main menu).
  if (off_match == nullptr) {
    OffscreenTarget* by_size = nullptr;
    for (auto& [k, t] : g_offscreen) {
      if (t.cleared_frame == ~0ull) {
        continue;
      }
      // Exact dimensions only: accepting a same-aspect pass broke the main menu.
      if (t.width != call.resolve_width || t.height != call.resolve_height) {
        continue;
      }
      // The same age bound as the stale path, or the warrior screen's 240x320 card-frame pass answers a main-menu resolve.
      if (stale_max >= 0 &&
          FramesSince(t.cleared_frame) > uint64_t(stale_max)) {
        continue;
      }
      if (by_size == nullptr || t.cleared_frame > by_size->cleared_frame) {
        by_size = &t;
      }
    }
    if (by_size != nullptr) {
      static std::set<uint32_t> reported_by_size;
      if (reported_by_size.insert(call.resolve_address).second) {
        KGFX_INFO("resolve {:08X} {}x{} matched a pass BY SIZE (surface "
                    "{:08X} never rendered here; the pass used a different surface object)",
                    call.resolve_address, call.resolve_width, call.resolve_height,
                    call.rt_surface);
      }
      off_match = by_size;
    }
  }
  return off_match;
}

}  // namespace

void ReplayResolveCommand(ReplayContext& rc, const DrawCall& call) {
  GraphicsSystem::DrawPathContext& ctx = rc.ctx;
  plume::RenderCommandList* list = rc.list;
  plume::RenderTexture* const target = rc.target;
  const plume::RenderFramebuffer* const framebuffer = rc.framebuffer;
  const uint32_t width = rc.width;
  const uint32_t height = rc.height;
  const uint32_t main_surface = rc.main_surface;
  // The source is the resolve's own surface, not whatever the last draw left active, which took the swap chain whenever the page pass went elsewhere or was dropped.
  plume::RenderTexture* source = rc.current_texture;
  plume::RenderTexture* resolve_depth_source = nullptr;
  // The bound target's real size, which PerformResolve clamps against; a sentinel for unknown read as unbounded and copied 1280x720 out of a 512x512 source.
  uint32_t src_w = width;
  uint32_t src_h = height;
  if (rc.current_key != 0) {
    auto bound_target = g_offscreen.find(rc.current_key);
    src_w = bound_target != g_offscreen.end() ? bound_target->second.width : 0;
    src_h = bound_target != g_offscreen.end() ? bound_target->second.height : 0;
  }
  // A resolve naming the scene's own surface reads the scene, not whichever offscreen pass is bound (that gave the screen-invert and heat haze a 512x512 post-process source).
  if (main_surface != 0 && call.rt_surface == main_surface) {
    source = target;
    resolve_depth_source = ctx.scene_depth;
    src_w = width;
    src_h = height;
  }
  if (call.rt_surface != 0 && main_surface != 0 && call.rt_surface != main_surface) {
    OffscreenTarget* off_match = FindOffscreenSource(call);
    if (off_match == nullptr) {
      // A real pass we failed to redirect: copying the frame buffer here painted the storybook pages purple, so the texture keeps what was last resolved into it.
      return;
    }
    source = off_match->color.get();
    resolve_depth_source = off_match->depth.get();
    src_w = off_match->width;
    src_h = off_match->height;
  }
  PerformResolve(ctx, list, source, resolve_depth_source, call, src_w, src_h, source == target);
  if ((call.resolve_clear_rt || call.resolve_clear_depth) && source && src_w && src_h) {
    const plume::RenderFramebuffer* clear_framebuffer = source == target ? framebuffer : nullptr;
    for (const auto& [key, off] : g_offscreen) {
      if (off.color.get() == source) {
        clear_framebuffer = off.framebuffer.get();
        break;
      }
    }
    if (call.resolve_depth && resolve_depth_source == nullptr) {
      auto depth_source = g_depthOnly.find((uint64_t(call.resolve_width) << 32) | call.resolve_height);
      clear_framebuffer = depth_source != g_depthOnly.end() ? depth_source->second.framebuffer.get() : nullptr;
      src_w = call.resolve_width;
      src_h = call.resolve_height;
    }
    if (clear_framebuffer) {
      float x = float(call.resolve_has_rect ? call.resolve_x : 0);
      float y = float(call.resolve_has_rect ? call.resolve_y : 0);
      float w = float(call.resolve_width);
      float h = float(call.resolve_height);
      if (clear_framebuffer == framebuffer) {
        float uv[4];
        GuestRectToHostUv(src_w, src_h, x, y, w, h, uv);
        x = uv[0] * src_w;
        y = uv[1] * src_h;
        w = uv[2] * src_w;
        h = uv[3] * src_h;
      }
      const plume::RenderRect rect(std::clamp(int32_t(std::floor(x)), 0, int32_t(src_w)), std::clamp(int32_t(std::floor(y)), 0, int32_t(src_h)), std::clamp(int32_t(std::ceil(x + w)), 0, int32_t(src_w)), std::clamp(int32_t(std::ceil(y + h)), 0, int32_t(src_h)));
      if (rect.right > rect.left && rect.bottom > rect.top) {
        list->setFramebuffer(clear_framebuffer);
        if (call.resolve_clear_rt && !call.resolve_depth) {
          const float* c = call.resolve_clear_color;
          list->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]), &rect, 1);
          if (source == target) InvalidateSceneColorResolve(ctx.scene_resolve_dirty);
        }
        if (call.resolve_clear_depth) list->clearDepthStencil(true, true, call.clear_depth, 0, &rect, 1);
      }
    }
  }
  list->setFramebuffer(rc.current_framebuffer);
  RebindSceneLayout(rc);
}

}  // namespace kameo::gpu::detail
