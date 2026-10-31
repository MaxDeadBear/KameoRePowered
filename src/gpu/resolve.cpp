#include "gpu/resolve.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

#include "gpu/config.h"
#include "gpu/draw/draw_constants.h"
#include "gpu/frame_profile.h"
#include "gpu/log.h"
#include "gpu/replay/screen_layout.h"
#include "gpu/resolve_blit.h"
#include "gpu/scene_resolve_state.h"
#include "gpu/resource_check.h"
#include "gpu/surface_pool.h"
#include "gpu/textures/texture_heap.h"
#include "gpu/textures/texture_upload.h"

namespace kameo::gpu::detail {

std::unordered_map<ResolveKey, CachedTexture, ResolveKeyHash> g_resolveTargets;
uint32_t g_depthResolveDone = 0;
uint32_t g_depthResolveMissed = 0;

void PerformResolve(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, plume::RenderTexture* source, plume::RenderTexture* depth_source, const DrawCall& call, uint32_t width, uint32_t height, bool source_is_scene) {
  // The depth path comes before the null-source guard: a shadow pass has no colour target, and that guard once discarded every shadow resolve (1017 per census window).
  if (call.resolve_depth) {
    // Shadow maps are depth-only passes matched by size, the only identity they have; a depth resolve taken with the colour target bound is the scene's own depth, which the heat haze reads.
    // The destination stays full-frame even when split-screen resolves one 640x720 viewport at a time.
    plume::RenderTexture* scene_depth = nullptr;
    if (call.rt_surface != 0 && source_is_scene && ctx.scene_depth != nullptr &&
        call.resolve_texture_width == kGuestWidth && call.resolve_texture_height == kGuestHeight) {
      scene_depth = ctx.scene_depth;
    }
    // A scene-depth copy stays at the window's resolution but covers only the guest frame's rectangle, which the draw cuts out when the scene is multisampled or boxed.
    float frame_uv[4];
    GuestRectToHostUv(width, height, 0.0f, 0.0f, float(kGuestWidth), float(kGuestHeight), frame_uv);
    const int32_t frame_x = int32_t(std::lround(frame_uv[0] * float(width)));
    const int32_t frame_y = int32_t(std::lround(frame_uv[1] * float(height)));
    const uint32_t frame_w = std::clamp(uint32_t(std::lround(frame_uv[2] * float(width))), 1u, width - uint32_t(frame_x));
    const uint32_t frame_h = std::clamp(uint32_t(std::lround(frame_uv[3] * float(height))), 1u, height - uint32_t(frame_y));
    const bool boxed = std::abs(int32_t(frame_w) - int32_t(width)) > 1 || std::abs(int32_t(frame_h) - int32_t(height)) > 1;
    // An offscreen depth resolve names one viewport-sized slot of a larger pass -- imposterModelRender draws each imposter into its own slot of the scratch pad and resolves depth with no rect -- so the slot is drawn out rather than the whole target copied.
    const bool depth_slot = scene_depth == nullptr && depth_source != nullptr && (width != call.resolve_width || height != call.resolve_height);
    int32_t slot_x = std::max(0, call.resolve_x);
    int32_t slot_y = std::max(0, call.resolve_y);
    if (depth_slot && !call.resolve_has_rect) {
      slot_x = int32_t(call.viewport[0]);
      slot_y = int32_t(call.viewport[1]);
    }
    const bool depth_by_draw = (scene_depth != nullptr && (ctx.scene_samples > 1 || boxed)) || depth_slot;
    const uint32_t depth_dest_width = scene_depth ? (depth_by_draw ? frame_w : width) : call.resolve_width;
    const uint32_t depth_dest_height = scene_depth ? (depth_by_draw ? frame_h : height) : call.resolve_height;
    auto depth_src = g_depthOnly.end();
    if (scene_depth == nullptr && depth_source == nullptr) {
      depth_src = g_depthOnly.find((uint64_t(call.resolve_width) << 32) | call.resolve_height);
    }
    if (scene_depth == nullptr && depth_source == nullptr && (depth_src == g_depthOnly.end() || !depth_src->second.depth)) {
      // Counted rather than logged once: the first miss happens at boot, long before any level target exists.
      ++g_depthResolveMissed;
      // Created now: the shadow maps resolve before any depth-only draw of that size exists, and once baked they are not re-resolved, so dropping this window left them magenta.
      EnsureDepthOnly(ctx, call.resolve_width, call.resolve_height);
      return;
    }
    ++g_depthResolveDone;

    const ResolveKey depth_key{call.resolve_address, uint16_t(call.resolve_texture_width),
                               uint16_t(call.resolve_texture_height), true};
    auto found_depth = g_resolveTargets.find(depth_key);
    if (found_depth != g_resolveTargets.end() && (found_depth->second.width != depth_dest_width || found_depth->second.height != depth_dest_height || (found_depth->second.framebuffer != nullptr) != depth_by_draw)) {
      // The window changed size; the old copy keeps its heap slot until the graveyard retires it.
      CachedTexture retired;
      retired.heap_index = found_depth->second.heap_index;
      retired.texture = std::move(found_depth->second.texture);
      retired.view = std::move(found_depth->second.view);
      retired.framebuffer = std::move(found_depth->second.framebuffer);
      g_textureGraveyard.queue(std::move(retired), ctx.frame_index);
      g_resolveTargets.erase(found_depth);
      found_depth = g_resolveTargets.end();
    }
    if (found_depth == g_resolveTargets.end()) {
      CachedTexture entry;
      // The sampleable depth copy: R32_FLOAT on D3D12 (D32's typeless group), but a depth target on Vulkan, where a copy's aspect masks must match and plume derives the aspect from DEPTH_TARGET.
      const bool depth_copy_needs_depth_dst = ctx.spirv && !depth_by_draw;
      const plume::RenderFormat depth_copy_format =
          depth_copy_needs_depth_dst ? plume::RenderFormat(ctx.depth_format)
                                     : plume::RenderFormat::R32_FLOAT;
      entry.texture = CreateTextureChecked(ctx.device,
          depth_by_draw ? plume::RenderTextureDesc::ColorTarget(depth_dest_width, depth_dest_height, plume::RenderFormat::R32_FLOAT)
          : depth_copy_needs_depth_dst
              ? plume::RenderTextureDesc::DepthTarget(depth_dest_width, depth_dest_height,
                                                      depth_copy_format)
              : plume::RenderTextureDesc::Texture2D(depth_dest_width, depth_dest_height, 1,
                                                    depth_copy_format),
          "depth resolve target");
      if (!entry.texture) {
        return;
      }
      if (depth_by_draw) {
        const plume::RenderTexture* attachment = entry.texture.get();
        plume::RenderFramebufferDesc fb;
        fb.colorAttachments = &attachment;
        fb.colorAttachmentsCount = 1;
        entry.framebuffer = CreateFramebufferChecked(ctx.device, fb, "depth resolve framebuffer");
        if (!entry.framebuffer) return;
      }
      entry.view = entry.texture->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(depth_copy_format));
      const uint32_t depth_slot = g_textureSlots.allocate();
      if (depth_slot == kNoSlot) {
        return;
      }
      entry.heap_index = depth_slot;
      entry.evictable = false;
      entry.width = depth_dest_width;
      entry.height = depth_dest_height;
      entry.format = depth_copy_format;
      ctx.texture_set->setTexture(entry.heap_index, entry.texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ, entry.view.get());
      KGFX_INFO("DEPTH resolve target {:08X} {}x{} registered at heap slot {}{}",
                  call.resolve_address, depth_dest_width, depth_dest_height, entry.heap_index,
                  scene_depth ? " (scene depth, at the window's resolution)" : "");
      found_depth = g_resolveTargets.emplace(depth_key, std::move(entry)).first;
    }

    // The source depth target must match too, or the full-resource copy is out of bounds from the other end.
    if (scene_depth == nullptr && depth_source != nullptr && !depth_slot &&
        (width != call.resolve_width || height != call.resolve_height)) {
      ++g_depthResolveMissed;
      return;
    }
    if (scene_depth == nullptr && depth_source == nullptr &&
        (depth_src->second.width != call.resolve_width || depth_src->second.height != call.resolve_height)) {
      ++g_depthResolveMissed;
      return;
    }

    plume::RenderTexture* depth_tex = scene_depth ? scene_depth : depth_source ? depth_source : depth_src->second.depth.get();
    if (scene_depth != nullptr) {
      static std::set<uint32_t> reported_scene_depth;
      if (reported_scene_depth.size() < 8 && reported_scene_depth.insert(call.resolve_address).second) {
        KGFX_INFO("DEPTH resolve {:08X} {}x{} takes the SCENE depth buffer (surface {:08X})",
                    call.resolve_address, call.resolve_width, call.resolve_height, call.rt_surface);
      }
    }
    plume::RenderTexture* depth_dst = found_depth->second.texture.get();
    if (depth_by_draw) {
      const bool drawn = depth_slot ? DrawDepthResolve(ctx, list, depth_tex, found_depth->second, slot_x, slot_y, false) : DrawDepthResolve(ctx, list, depth_tex, found_depth->second, frame_x, frame_y, ctx.scene_samples > 1);
      if (!drawn) return;
    } else {
      const plume::RenderTextureBarrier to_copy[] = {
          plume::RenderTextureBarrier(depth_tex, plume::RenderTextureLayout::COPY_SOURCE),
          plume::RenderTextureBarrier(depth_dst, plume::RenderTextureLayout::COPY_DEST),
      };
      list->barriers(plume::RenderBarrierStage::COPY, to_copy, uint32_t(std::size(to_copy)));
      list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(depth_dst, 0, 0),
                              plume::RenderTextureCopyLocation::Subresource(depth_tex, 0, 0), 0, 0,
                              0, nullptr);
    }
    const plume::RenderTextureBarrier back[] = {
        plume::RenderTextureBarrier(depth_tex, plume::RenderTextureLayout::DEPTH_WRITE),
        plume::RenderTextureBarrier(depth_dst, plume::RenderTextureLayout::SHADER_READ),
    };
    list->barriers(plume::RenderBarrierStage::GRAPHICS, back, uint32_t(std::size(back)));
    found_depth->second.written = true;
    ++g_textureMemoGeneration;
    return;
  }

  if (!source) {
    return;  // colour resolve with nothing bound to copy from
  }
  // A multisampled scene cannot be sampled or copied directly, so the resolve works from a single-sample copy of it.
  plume::RenderTexture* multisampled_scene = nullptr;
  if (source_is_scene && ctx.scene_samples > 1 && ctx.scene_resolved != nullptr) {
    const bool resolved = ResolveSceneColorIfDirty(ctx.scene_resolve_dirty, [&] {
      const plume::RenderTextureBarrier to_resolve[] = {
          plume::RenderTextureBarrier(source, plume::RenderTextureLayout::RESOLVE_SOURCE),
          plume::RenderTextureBarrier(ctx.scene_resolved, plume::RenderTextureLayout::RESOLVE_DEST),
      };
      list->barriers(plume::RenderBarrierStage::COPY, to_resolve, uint32_t(std::size(to_resolve)));
      list->resolveTexture(ctx.scene_resolved, source);
    });
    ++(resolved ? g_frameProfile.msaa_resolves : g_frameProfile.msaa_resolve_reuses);
    multisampled_scene = source;
    source = ctx.scene_resolved;
  }


  const uint32_t texture_width = call.resolve_texture_width ? call.resolve_texture_width : call.resolve_width;
  const uint32_t texture_height = call.resolve_texture_height ? call.resolve_texture_height : call.resolve_height;
  const ResolveKey colour_key{call.resolve_address, uint16_t(texture_width), uint16_t(texture_height), false};
  // The post chain resolves the whole frame and composites it back over the screen, so a guest-sized copy would squeeze the scene through 720p whatever the window is.
  // The guest names an explicit 1280x720 rect here, so the test is that the rect covers the whole guest frame, not that it is absent.
  const bool full_scene_copy = source_is_scene && call.resolve_x <= 0 && call.resolve_y <= 0 && call.resolve_dest_x <= 0 && call.resolve_dest_y <= 0 && call.resolve_width == kGuestWidth && call.resolve_height == kGuestHeight && texture_width == kGuestWidth && texture_height == kGuestHeight && REXCVAR_GET(kameo_gfx_native_resolve);
  const uint32_t dest_width = full_scene_copy ? width : texture_width;
  const uint32_t dest_height = full_scene_copy ? height : texture_height;
  const bool wide = !source_is_scene && (WideGuestFormat(call.resolve_format) || g_wideResolveAddresses.count(call.resolve_address));
  {
    static std::set<uint64_t> reported_formats;
    if (reported_formats.size() < 64 && reported_formats.insert((uint64_t(call.resolve_address) << 8) | call.resolve_format).second && g_wideResolveAddresses.count(call.resolve_address)) KGFX_INFO("resolve {:08X}: destination header format {}, sampled wide", call.resolve_address, call.resolve_format);
  }
  if (wide && call.rt_surface != 0 && g_wideSurfaces.insert(call.rt_surface).second) KGFX_INFO("surface {:08X} resolves into wide format {} ({:08X} {}x{}); its target and copies become 16-bit float", call.rt_surface, call.resolve_format, call.resolve_address, call.resolve_width, call.resolve_height);
  const plume::RenderFormat dest_format = wide ? kWideColorFormat : plume::RenderFormat::B8G8R8A8_UNORM;
  plume::RenderFormat source_format = plume::RenderFormat(ctx.color_format);
  for (const auto& [key, target] : g_offscreen) {
    if (target.color.get() == source) source_format = target.format;
  }
  auto found = g_resolveTargets.find(colour_key);
  if (found != g_resolveTargets.end() && (found->second.width != dest_width || found->second.height != dest_height || found->second.format != dest_format)) {
    // The window changed size. The old texture keeps its heap slot until the graveyard retires it, so the replacement takes a fresh one.
    CachedTexture retired;
    retired.heap_index = found->second.heap_index;
    retired.texture = std::move(found->second.texture);
    retired.view = std::move(found->second.view);
    retired.framebuffer = std::move(found->second.framebuffer);
    g_textureGraveyard.queue(std::move(retired), ctx.frame_index);
    g_resolveTargets.erase(found);
    found = g_resolveTargets.end();
  }
  if (found == g_resolveTargets.end()) {
    CachedTexture entry;
    // A render target matching the swap chain's format: a resolve out of a scaled scene has to be drawn, and B8G8R8A8 still samples red in .r.
    entry.texture = CreateTextureChecked(ctx.device, plume::RenderTextureDesc::ColorTarget(dest_width, dest_height, dest_format), "resolve target");
    if (!entry.texture) {
      return;
    }
    entry.view = entry.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(dest_format));
    const plume::RenderTexture* resolve_attachments[] = {entry.texture.get()};
    plume::RenderFramebufferDesc resolve_fb;
    resolve_fb.colorAttachments = resolve_attachments;
    resolve_fb.colorAttachmentsCount = 1;
    entry.framebuffer = CreateFramebufferChecked(ctx.device, resolve_fb, "resolve framebuffer");
    if (!entry.framebuffer) return;
    const uint32_t resolve_slot = g_textureSlots.allocate();
    if (resolve_slot == kNoSlot) {
      return;
    }
    entry.heap_index = resolve_slot;
    entry.evictable = false;
    entry.width = dest_width;
    entry.height = dest_height;
    entry.format = dest_format;
    ctx.texture_set->setTexture(entry.heap_index, entry.texture.get(),
                                plume::RenderTextureLayout::SHADER_READ, entry.view.get());
    KGFX_INFO("resolve target {:08X} {}x{} registered at heap slot {}{}",
                call.resolve_address, dest_width, dest_height, entry.heap_index,
                full_scene_copy ? " (scene, at the window's resolution)" : "");
    found = g_resolveTargets.emplace(colour_key, std::move(entry)).first;
  }

  plume::RenderTexture* dest = found->second.texture.get();

  // Without an explicit rect the guest means the current viewport: imposterModelRender moves it between the 21 slots of one 128x128 scratch pad, resolving each into its own texture.
  int32_t src_x = std::max(0, call.resolve_x);
  int32_t src_y = std::max(0, call.resolve_y);
  uint32_t src_w = call.resolve_width;
  uint32_t src_h = call.resolve_height;
  if (!call.resolve_has_rect) {
    const int32_t vp_x = int32_t(call.viewport[0]);
    const int32_t vp_y = int32_t(call.viewport[1]);
    const uint32_t vp_w = uint32_t(std::max(0.0f, call.viewport[2]));
    const uint32_t vp_h = uint32_t(std::max(0.0f, call.viewport[3]));
    const bool sub_rect = vp_w != 0 && vp_h != 0 &&
                          (vp_x != 0 || vp_y != 0 || vp_w != width || vp_h != height);
    if (sub_rect && uint32_t(vp_x) < width && uint32_t(vp_y) < height) {
      src_x = vp_x;
      src_y = vp_y;
      src_w = vp_w;
      src_h = vp_h;
    } else {
      // No rect and no viewport naming a slot: Resolve takes the whole surface (a destination-sized corner of a 256x256 pad was its empty top-left quarter).
      src_x = 0;
      src_y = 0;
      src_w = width;
      src_h = height;
    }
  }

  // Clamp to what both ends hold, offset included.
  const int32_t left_pre = src_x;
  const int32_t top_pre = src_y;
  const uint32_t src_avail_w = uint32_t(left_pre) < width ? width - uint32_t(left_pre) : 0;
  const uint32_t src_avail_h = uint32_t(top_pre) < height ? height - uint32_t(top_pre) : 0;
  // What the guest asked to read, clamped to the source; copy_w/copy_h clamp it to the destination too, which is only right for a plain copy.
  const uint32_t want_w = std::min(src_w, src_avail_w);
  const uint32_t want_h = std::min(src_h, src_avail_h);
  const int32_t dest_x = std::max(0, call.resolve_dest_x);
  const int32_t dest_y = std::max(0, call.resolve_dest_y);
  const uint32_t dest_avail_w = uint32_t(dest_x) < found->second.width ? found->second.width - uint32_t(dest_x) : 0;
  const uint32_t dest_avail_h = uint32_t(dest_y) < found->second.height ? found->second.height - uint32_t(dest_y) : 0;
  const uint32_t copy_w = std::min(want_w, dest_avail_w);
  const uint32_t copy_h = std::min(want_h, dest_avail_h);
  if (copy_w == 0 || copy_h == 0) {
    return;
  }



  // The scene is the guest frame scaled to the window, so a copy would take a corner of it; it is drawn through the same transform the draw loop used.
  bool blitted = false;
  if (source_is_scene && (width != kGuestWidth || height != kGuestHeight)) {
    float uv[4];
    GuestRectToHostUv(width, height, float(left_pre), float(top_pre), float(call.resolve_width),
                      float(call.resolve_height), uv);
    blitted = BlitScaledResolve(ctx, list, source, found->second, uv,
                                full_scene_copy ? 0 : dest_x, full_scene_copy ? 0 : dest_y,
                                full_scene_copy ? found->second.width : copy_w,
                                full_scene_copy ? found->second.height : copy_h);
  } else if (want_w != found->second.width || want_h != found->second.height || source_format != found->second.format) {
    // CopyTextureRegion cannot scale: too small left 72% of the shadow troll's screen-invert target unwritten, too large took the empty corner of the imposter pad.
    const float uv[4] = {float(left_pre) / float(width), float(top_pre) / float(height),
                         float(want_w) / float(width), float(want_h) / float(height)};
    blitted = BlitScaledResolve(ctx, list, source, found->second, uv, dest_x, dest_y, copy_w, copy_h);
    static std::set<uint64_t> reported;
    const uint64_t k = (uint64_t(want_w) << 48) ^ (uint64_t(want_h) << 32) ^
                       (uint64_t(found->second.width) << 16) ^ found->second.height;
    if (blitted && reported.size() < 8 && reported.insert(k).second) {
      KGFX_INFO("resolve {:08X}: {}x{} source region scaled into {}x{} dest rather than cropped",
                  call.resolve_address, want_w, want_h, found->second.width,
                  found->second.height);
    }
  }

  // A copy between different formats is rejected outright and removes the device.
  if (!blitted && source_format != found->second.format) return;
  if (!blitted) {
    const plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dest, plume::RenderTextureLayout::COPY_DEST),
    };
    list->barriers(plume::RenderBarrierStage::COPY, to_copy, uint32_t(std::size(to_copy)));

    // RenderBox is (left, top, right, bottom, front, back), not two 3D points.
    const int32_t left = left_pre;
    const int32_t top = top_pre;
    const plume::RenderBox box(left, top, left + int32_t(copy_w), top + int32_t(copy_h), 0, 1);
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dest, 0, 0),
                            plume::RenderTextureCopyLocation::Subresource(source, 0, 0), dest_x, dest_y, 0,
                            &box);
  }

  const plume::RenderTextureBarrier back[] = {
      plume::RenderTextureBarrier(multisampled_scene ? multisampled_scene : source, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(dest, plume::RenderTextureLayout::SHADER_READ),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, back, uint32_t(std::size(back)));
  found->second.written = true;
  ++g_textureMemoGeneration;
}

void ReleaseResolveTargets() { g_resolveTargets.clear(); }

}  // namespace kameo::gpu::detail
