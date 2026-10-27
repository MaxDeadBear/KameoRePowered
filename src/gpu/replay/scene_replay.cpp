#include "gpu/replay/replay_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "gpu/config.h"
#include "gpu/draw/capture.h"
#include "gpu/draw/draw_constants.h"
#include "gpu/draw/vertex_capture.h"
#include "gpu/draw_path.h"
#include "gpu/frame_profile.h"
#include "gpu/frame_ring.h"
#include "gpu/device_removed.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/replay/book_pick.h"
#include "gpu/replay/screen_layout.h"
#include "gpu/resolve_blit.h"
#include "gpu/sampler_cache.h"
#include "gpu/shaders/embedded_shaders.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/stereo/stereo.h"  // STEREO:
#include "gpu/textures/texture_cache.h"
#include "gpu/textures/texture_heap.h"
#include "gpu/textures/texture_replacement.h"
#include "gpu/textures/texture_upload.h"

namespace kameo::gpu::detail {

uint32_t g_sceneFrames = 0;
uint32_t g_sceneEye = 0;  // STEREO:

ReplayContext::ReplayContext(GraphicsSystem& gfx, const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, DrawFrame& frame, plume::RenderTexture* target, const plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height)
    : gfx(gfx), ctx(ctx), list(list), frame(frame), base(frame.guest_base), target(target), framebuffer(framebuffer), width(width), height(height), current_texture(target), current_framebuffer(framebuffer), current_color_format(plume::RenderFormat(ctx.color_format)) {}

void BindSceneDescriptors(plume::RenderCommandList* list, const GraphicsSystem::DrawPathContext& ctx) {
  list->setGraphicsPipelineLayout(ctx.layout);
  list->setGraphicsDescriptorSet(ctx.texture_set, 0);
  list->setGraphicsDescriptorSet(ctx.texture3d_set, 1);
  list->setGraphicsDescriptorSet(ctx.cube_set, 2);
  list->setGraphicsDescriptorSet(ctx.sampler_set, 3);
}

void RebindSceneLayout(ReplayContext& rc) {
  rc.bound.reset();
  BindSceneDescriptors(rc.list, rc.ctx);
}

namespace {

// Where a draw's geometry and constant blocks went: a resident buffer or a ring, at an offset.
struct DrawUploads {
  plume::RenderBuffer* vertex_buffer = nullptr;
  uint32_t vertex_offset = kNoData;
  plume::RenderBuffer* vertex_resident = nullptr;
  plume::RenderBuffer* index_buffer = nullptr;
  uint32_t index_offset = kNoData;
  plume::RenderBuffer* index_resident = nullptr;
  uint32_t vs_offset = kNoData;
  uint32_t ps_offset = kNoData;
  uint32_t shared_offset = kNoData;
};

// The draw's viewport on its target, kept for the book pick and the traces.
struct DrawPlacement {
  bool on_screen = false;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float vp_x = 0.0f;
  float vp_y = 0.0f;
  float vp_width = 0.0f;
  float vp_height = 0.0f;
};

// Writes the draw's texture and sampler indices and shared constants into the shared block; true when it samples the storybook page.
// Whether this frame's draws time their texture, pipeline and upload work; read once per frame.
bool g_profileDraws = false;

bool BuildSharedBlock(ReplayContext& rc, const DrawCall& call) {
  GraphicsSystem::DrawPathContext& ctx = rc.ctx;
  plume::RenderCommandList* list = rc.list;
  uint8_t* base = rc.base;
  auto& shared = rc.shared;
  const bool book_pick_wanted = rc.book_pick_wanted;
  std::memset(shared, 0, sizeof(shared));
  auto* shared_words = reinterpret_cast<uint32_t*>(shared);
  bool samples_book_page = false;
  for (uint32_t s = 0; s < kSamplerCount; ++s) {
    const GuestTextureFetch& fetch = call.textures[s];
    if (!fetch.valid()) {
      continue;
    }
    uint32_t dimension_mask = 0;
    if (call.ps) {
      dimension_mask |= call.ps->textureDimensionMask;
    }
    if (call.vs) {
      dimension_mask |= call.vs->textureDimensionMask;
    }
    uint32_t want_dimension = (dimension_mask >> (s * 2)) & 3u;
    if (want_dimension > 2) {
      want_dimension = 0;
    }
    bool volume = false;
    uint32_t texture_index = EnsureTexture(base, ctx, list, fetch, call.texture_objects[s], call.texture_addresses[s], want_dimension, &volume);
    // DXN is the game's normal map format, so with bump mapping off those samples read the flat normal instead.
    if (texture_index != 0 && want_dimension == 0 && g_flatNormalSlot != 0 && fetch.format() == kXenosFormat_DXN && !REXCVAR_GET(kameo_gfx_normal_maps)) texture_index = g_flatNormalSlot;
    // A fetch constant with no texture object behind it is a slot the guest left empty, which samples as zero on the console.
    if (texture_index == 0 && call.texture_objects[s] == 0 && want_dimension == 0 && g_emptySlot != 0) texture_index = g_emptySlot;
    if (texture_index == 0) {
      continue;
    }
    if (book_pick_wanted && want_dimension == 0 && !samples_book_page) samples_book_page = IsBookPageTexture(texture_index);
    // XenosRecomp's layout: 16 Texture2D indices, 16 Texture3D, 16 TextureCube, 16 sampler indices, then the shared constants at c16; a volume sample through a zeroed Texture3D slot read slot 0's 2D view as zero.
    shared_words[want_dimension * 16 + s] = texture_index;
    // The two shadow-map texture objects shadowRender resolves into.
    const bool shadow_map = call.texture_objects[s] == 0x8286F160 || call.texture_objects[s] == 0x8286F138;
    shared_words[48 + s] = EnsureSampler(ctx, fetch, shadow_map && REXCVAR_GET(kameo_gfx_shadow_filtering) && ShadowMapsFilterable(ctx));
  }
  shared_words[64] = call.booleans;                       // c16.x g_Booleans
  shared_words[65] = 0;                                   // c16.y g_SwappedTexcoords
  // PA_SU_VTX_CNTL selects integer guest pixel centers at zero, requiring a positive half-pixel window shift before host rasterization.
  const auto half_pixel = state::PixelCenterOffset(call.half_pixel, call.viewport[2], call.viewport[3]);
  std::memcpy(&shared_words[66], half_pixel.data(), sizeof(float) * half_pixel.size());
  std::memcpy(&shared_words[68], &call.alpha_ref, sizeof(float));  // c17.x g_AlphaThreshold
  // STEREO: c17.yz g_StereoShift, the eye's shift for draws seen through the main camera; zero otherwise.
  if (rc.stereo_shift[0] != 0.0f && StereoShiftsDraw(call, rc.frame, rc.current_framebuffer == rc.framebuffer)) std::memcpy(&shared_words[69], rc.stereo_shift, sizeof(rc.stereo_shift));
  if (FindPointSprite(call)) {
    const float point[4] = {1.0f / std::max(call.viewport[2], 1.0f), 1.0f / std::max(call.viewport[3], 1.0f), float(call.point_minmax & 0xFFFFu) * 0.125f, float(call.point_minmax >> 16) * 0.125f};
    std::memcpy(&shared_words[72], point, sizeof(point));
  }
  return samples_book_page;
}

// Puts the draw's geometry and constant blocks where the GPU reads them; false drops the draw for want of upload space.
bool UploadDrawData(ReplayContext& rc, const DrawCall& call, DrawUploads* out) {
  GraphicsSystem::DrawPathContext& ctx = rc.ctx;
  DrawFrame& frame = rc.frame;
  auto& shared = rc.shared;
  auto& shared_prev = rc.shared_prev;
  uint32_t& shared_prev_offset = rc.shared_prev_offset;
  bool& shared_prev_valid = rc.shared_prev_valid;
  plume::RenderBuffer* vertex_buffer = g_vertexRing.buffer.get();
  uint32_t vertex_offset = kNoData;
  plume::RenderBuffer* vertex_resident = call.vertex_blob ? ResidentBufferFor(ctx, call.vertex_blob, call.vertex_blob->data(), call.vertex_bytes, plume::RenderBufferFlag::VERTEX) : nullptr;
  if (vertex_resident) {
    vertex_buffer = vertex_resident;
    vertex_offset = 0;
  } else {
    vertex_offset = g_vertexRing.writeOnce(VertexKey(call), VertexBytes(frame, call), call.vertex_bytes, 16);
  }
  plume::RenderBuffer* index_buffer = g_indexRing.buffer.get();
  uint32_t index_offset = kNoData;
  plume::RenderBuffer* index_resident = call.index_blob ? ResidentBufferFor(ctx, call.index_blob, call.index_blob->data(), call.index_count * 4, plume::RenderBufferFlag::INDEX) : nullptr;
  if (index_resident) {
    index_buffer = index_resident;
    index_offset = 0;
  } else {
    index_offset = g_indexRing.writeOnce(IndexKey(call), IndexBytes(frame, call), call.index_count * 4, 4);
  }
  const uint32_t vs_offset = call.vs_ring != kNoData ? call.vs_ring : g_constantRing.writeOnce(call.vs_constants, frame.arena.data() + call.vs_constants, call.vs_constant_bytes, kRootConstantAlign);
  uint32_t ps_offset = kNoData;
  const bool strip_specular = !REXCVAR_GET(kameo_gfx_specular);
  const bool strip_bump = !REXCVAR_GET(kameo_gfx_bump_maps);
  const ShaderMaterialRegisters materials = (strip_specular || strip_bump) && call.ps_constants != kNoData ? ShaderMaterialRegistersOf(call.ps) : ShaderMaterialRegisters{};
  const bool patch_specular = strip_specular && materials.specular_power >= 0 && uint32_t(materials.specular_power + 1) * 16 <= call.ps_constant_bytes;
  const bool patch_bump = strip_bump && materials.bump_count > 0;
  if (patch_specular || patch_bump) {
    // A patched copy under its own key, so the shared block other draws dedupe against stays untouched.
    static thread_local std::vector<float> patched;
    patched.resize(call.ps_constant_bytes / 4);
    std::memcpy(patched.data(), frame.arena.data() + call.ps_constants, patched.size() * 4);
    // c_power is the exponent of x^c_power for x in [0, 1], so a huge one sends the highlight to zero.
    if (patch_specular) patched[size_t(materials.specular_power) * 4] = 4096.0f;
    for (uint8_t i = 0; patch_bump && i < materials.bump_count; ++i) {
      if (uint32_t(materials.bump[i] + 1) * 16 <= call.ps_constant_bytes) std::fill_n(patched.begin() + size_t(materials.bump[i]) * 4, 4, 0.0f);
    }
    const uint64_t key = (1ull << 63) | (uint64_t(call.ps_constants) << 20) | (uint64_t(size_t(call.ps - g_shaderCacheEntries)) << 2) | (patch_specular ? 1u : 0u) | (patch_bump ? 2u : 0u);
    ps_offset = g_constantRing.writeOnce(key, patched.data(), call.ps_constant_bytes, kRootConstantAlign);
  } else {
    ps_offset = call.ps_ring != kNoData ? call.ps_ring : g_constantRing.writeOnce(call.ps_constants, frame.arena.data() + call.ps_constants, call.ps_constant_bytes, kRootConstantAlign);
  }
  uint32_t shared_offset = shared_prev_offset;
  if (!shared_prev_valid || std::memcmp(shared, shared_prev, sizeof(shared)) != 0) {
    shared_offset = g_constantRing.write(shared, sizeof(shared), kRootConstantAlign);
    if (shared_offset != kNoData) {
      std::memcpy(shared_prev, shared, sizeof(shared));
      shared_prev_offset = shared_offset;
      shared_prev_valid = true;
    }
  }
  if (vertex_offset == kNoData || index_offset == kNoData || vs_offset == kNoData ||
      ps_offset == kNoData || shared_offset == kNoData) {
    return false;
  }
  *out = {vertex_buffer, vertex_offset, vertex_resident, index_buffer, index_offset, index_resident, vs_offset, ps_offset, shared_offset};
  return true;
}

// Binds the pipeline, constant blocks and geometry, skipping what the list already carries; false drops a draw whose views would run past a ring.
bool BindDraw(ReplayContext& rc, const DrawCall& call, plume::RenderPipeline* pipeline, const DrawUploads& up) {
  GraphicsSystem::DrawPathContext& ctx = rc.ctx;
  plume::RenderCommandList* list = rc.list;
  BoundState& bound = rc.bound;
  plume::RenderBuffer* const vertex_buffer = up.vertex_buffer;
  const uint32_t vertex_offset = up.vertex_offset;
  plume::RenderBuffer* const vertex_resident = up.vertex_resident;
  plume::RenderBuffer* const index_buffer = up.index_buffer;
  const uint32_t index_offset = up.index_offset;
  plume::RenderBuffer* const index_resident = up.index_resident;
  const uint32_t vs_offset = up.vs_offset;
  const uint32_t ps_offset = up.ps_offset;
  const uint32_t shared_offset = up.shared_offset;
  if (pipeline != bound.pipeline) {
    bound.pipeline = pipeline;
    list->setPipeline(pipeline);
  }
  if (vs_offset != bound.vs_offset || ps_offset != bound.ps_offset ||
      shared_offset != bound.shared_offset) {
    bound.vs_offset = vs_offset;
    bound.ps_offset = ps_offset;
    bound.shared_offset = shared_offset;
    if (ctx.spirv) {
      // The SPIR-V shaders read the three blocks through vk::RawBufferLoad off raw GPU addresses, pushed after setPipeline because pushing is scoped to the bound layout.
      const uint64_t base = g_constantRing.buffer->getDeviceAddress();
      const uint64_t push[3] = {base + vs_offset, base + ps_offset, base + shared_offset};
      list->setGraphicsPushConstants(0, push, 0, uint32_t(sizeof(push)));
    } else {
      list->setGraphicsRootDescriptor(g_constantRing.buffer->at(vs_offset), 0);
      list->setGraphicsRootDescriptor(g_constantRing.buffer->at(ps_offset), 1);
      list->setGraphicsRootDescriptor(g_constantRing.buffer->at(shared_offset), 2);
    }
  }

  // Never record a view that runs past its ring: it poisons the command list and the device dies at submit, far from the cause.
  if ((!vertex_resident && uint64_t(vertex_offset) + call.vertex_bytes > g_vertexRing.capacity) ||
      (!index_resident && uint64_t(index_offset) + uint64_t(call.index_count) * 4 > g_indexRing.capacity)) {
    static uint32_t reported = 0;
    if (reported < 8) {
      ++reported;
      KGFX_ERROR("draw DROPPED: buffer view would run past its ring "
                   "(vtx {}+{} of {}, idx {}+{} of {}). Recording it would poison the command "
                   "list and remove the device at ExecuteCommandLists.",
                   vertex_offset, call.vertex_bytes, g_vertexRing.capacity, index_offset,
                   call.index_count * 4, g_indexRing.capacity);
    }
    return false;
  }

  const uint32_t index_bytes = call.index_count * 4;
  if (vertex_buffer != bound.vertex_buffer || vertex_offset != bound.vertex_offset || call.vertex_bytes != bound.vertex_bytes ||
      call.vertex_stride != bound.vertex_stride || index_buffer != bound.index_buffer || index_offset != bound.index_offset ||
      index_bytes != bound.index_bytes) {
    bound.vertex_buffer = vertex_buffer;
    bound.vertex_offset = vertex_offset;
    bound.vertex_bytes = call.vertex_bytes;
    bound.vertex_stride = call.vertex_stride;
    bound.index_buffer = index_buffer;
    bound.index_offset = index_offset;
    bound.index_bytes = index_bytes;
    const plume::RenderVertexBufferView vertex_view(vertex_buffer->at(vertex_offset), call.vertex_bytes);
    const plume::RenderInputSlot slot(0, call.vertex_stride);
    list->setVertexBuffers(0, &vertex_view, 1, &slot);

    const plume::RenderIndexBufferView index_view(index_buffer->at(index_offset), index_bytes, plume::RenderFormat::R32_UINT);
    list->setIndexBuffer(&index_view);
  }
  return true;
}

// The guest's viewport scaled to where the draw lands, uniformly and centred; a pass routed offscreen whose target failed still lands on the scene.
DrawPlacement SetDrawViewport(ReplayContext& rc, const DrawCall& call, uint32_t off_w, uint32_t off_h) {
  plume::RenderCommandList* list = rc.list;
  BoundState& bound = rc.bound;
  const DrawFrame& frame = rc.frame;
  const uint32_t width = rc.width;
  const uint32_t height = rc.height;
  const bool on_screen = rc.current_framebuffer == rc.framebuffer;
  const GuestToHost to_host = on_screen ? GuestToHostTransform(width, height) : GuestToHost{};
  const float scale_x = to_host.scale_x;
  const float scale_y = to_host.scale_y;
  const float origin_x = to_host.origin_x;
  const float origin_y = to_host.origin_y;
  float vp_width = (call.viewport[2] > 0 ? call.viewport[2] : float(kGuestWidth)) * scale_x;
  float vp_height =
      (call.viewport[3] > 0 ? call.viewport[3] : float(kGuestHeight)) * scale_y;
  float vp_x = origin_x + call.viewport[0] * scale_x;
  float vp_y = origin_y + call.viewport[1] * scale_y;
  // A HUD draw placed by PlaceHudBox gets a viewport of its own from its clip-space affine, and its scissor follows the same map.
  HudPlacement hud;
  const size_t draw_index = size_t(&call - frame.draws.data());
  const bool hud_placed = on_screen && !rc.hud_boxed.empty() && rc.hud_boxed[draw_index] && PlaceHudBox(rc.hud_boxes[draw_index].data(), scale_x / scale_y, &hud);
  const float guest_vp_x = vp_x;
  const float guest_vp_y = vp_y;
  if (hud_placed) {
    vp_x = guest_vp_x + (hud.bx + 1.0f - hud.mx) * 0.5f * vp_width;
    vp_y = guest_vp_y + (1.0f - hud.by - hud.my) * 0.5f * vp_height;
    vp_width *= hud.mx;
    vp_height *= hud.my;
  }
  if (!bound.viewport_valid || vp_x != bound.viewport[0] || vp_y != bound.viewport[1] ||
      vp_width != bound.viewport[2] || vp_height != bound.viewport[3] ||
      call.viewport[4] != bound.depth_range[0] || call.viewport[5] != bound.depth_range[1]) {
    bound.viewport_valid = true;
    bound.viewport[0] = vp_x;
    bound.viewport[1] = vp_y;
    bound.viewport[2] = vp_width;
    bound.viewport[3] = vp_height;
    bound.depth_range[0] = call.viewport[4];
    bound.depth_range[1] = call.viewport[5];
    list->setViewports(plume::RenderViewport(vp_x, vp_y, vp_width, vp_height, call.viewport[4],
                                             call.viewport[5]));
  }
  const uint32_t sc_w = on_screen ? width : off_w;
  const uint32_t sc_h = on_screen ? height : off_h;
  // The guest's scissor goes through the same transform as its viewport; the text windows clip the lines scrolling past their edges with it.
  int32_t scissor[4] = {0, 0, int32_t(sc_w), int32_t(sc_h)};
  if (call.scissor_enable) {
    float edges[4] = {origin_x + float(call.scissor[0]) * scale_x, origin_y + float(call.scissor[1]) * scale_y, origin_x + float(call.scissor[2]) * scale_x, origin_y + float(call.scissor[3]) * scale_y};
    if (hud_placed) {
      edges[0] = vp_x + (edges[0] - guest_vp_x) * hud.mx;
      edges[2] = vp_x + (edges[2] - guest_vp_x) * hud.mx;
      edges[1] = vp_y + (edges[1] - guest_vp_y) * hud.my;
      edges[3] = vp_y + (edges[3] - guest_vp_y) * hud.my;
    }
    scissor[0] = std::clamp(int32_t(std::floor(edges[0])), 0, int32_t(sc_w));
    scissor[1] = std::clamp(int32_t(std::floor(edges[1])), 0, int32_t(sc_h));
    scissor[2] = std::clamp(int32_t(std::ceil(edges[2])), scissor[0], int32_t(sc_w));
    scissor[3] = std::clamp(int32_t(std::ceil(edges[3])), scissor[1], int32_t(sc_h));
  }
  if (!bound.scissor_valid || std::memcmp(scissor, bound.scissor, sizeof(scissor)) != 0) {
    bound.scissor_valid = true;
    std::memcpy(bound.scissor, scissor, sizeof(scissor));
    list->setScissors(plume::RenderRect(scissor[0], scissor[1], scissor[2], scissor[3]));
  }
  return {on_screen, scale_x, scale_y, origin_x, origin_y, vp_x, vp_y, vp_width, vp_height};
}

void ReplayDraw(ReplayContext& rc, const DrawCall& call, uint32_t off_w, uint32_t off_h) {
  // Both cull bits set means cull every face, the guest drawing nothing; plume has no FRONT_AND_BACK, so the pipeline would draw back faces instead.
  const bool cull_all = (call.mode_control & state::kCullFrontBit) != 0 &&
                        (call.mode_control & state::kCullBackBit) != 0;
  if (cull_all) {
    if (REXCVAR_GET(kameo_gfx_cull_both)) {
      return;
    }
  }

  plume::RenderShader* vs = rc.gfx.ShaderModule(call.vs);
  plume::RenderShader* ps = (call.depth_only || call.stencil_only) ? nullptr : rc.gfx.ShaderModule(call.ps);
  if (!vs || (!ps && !call.depth_only && !call.stencil_only)) {
    return;
  }
  plume::RenderPipeline* pipeline = nullptr;
  {
    ScopedNs timed(g_profileDraws ? &g_frameProfile.ns_pipelines : nullptr);
    pipeline = EnsurePipeline(rc.ctx, call, vs, ps, rc.current_framebuffer == rc.framebuffer ? rc.ctx.scene_samples : 1, rc.current_color_format);
  }
  if (!pipeline) {
    return;
  }

  // Workaround: the FMV orb's white disk hides the video on our side although the console shows the video through it, and the guest issues it identically.
  if (call.ps && call.ps->hash == 0x3547764B6B934FEEull && call.index_count == 192 && call.vertex_count == 65) return;
  bool samples_book_page = false;
  {
    ScopedNs timed(g_profileDraws ? &g_frameProfile.ns_textures : nullptr);
    samples_book_page = BuildSharedBlock(rc, call);
  }
  DrawUploads up;
  {
    ScopedNs timed(g_profileDraws ? &g_frameProfile.ns_uploads : nullptr);
    if (!UploadDrawData(rc, call, &up)) return;
    if (!BindDraw(rc, call, pipeline, up)) return;
  }
  const DrawPlacement placement = SetDrawViewport(rc, call, off_w, off_h);
  rc.list->drawIndexedInstanced(call.index_count, 1, 0, 0, 0);
  ++g_frameProfile.draws;
  if (samples_book_page && placement.on_screen && !call.depth_only && !FindPointSprite(call) && call.layout && BookPickHasRoom()) {
    BookPickDraw pick;
    pick.vs = vs;
    pick.call = &call;
    pick.vertex_buffer = up.vertex_buffer;
    pick.vertex_offset = up.vertex_offset;
    pick.index_buffer = up.index_buffer;
    pick.index_offset = up.index_offset;
    pick.vs_offset = up.vs_offset;
    pick.ps_offset = up.ps_offset;
    pick.shared_offset = up.shared_offset;
    pick.viewport[0] = placement.vp_x;
    pick.viewport[1] = placement.vp_y;
    pick.viewport[2] = placement.vp_width;
    pick.viewport[3] = placement.vp_height;
    pick.viewport[4] = call.viewport[4];
    pick.viewport[5] = call.viewport[5];
    AddBookPickDraw(pick);
  }
}

// A draw or depth clear: routes it to its target, then replays it.
void ReplayCommand(ReplayContext& rc, const DrawCall& call) {
  uint32_t off_w = 0;
  uint32_t off_h = 0;
  if (!RouteDrawTarget(rc, call, off_w, off_h)) return;
  if (call.kind == CommandKind::DepthClear) {
    ReplayDepthClear(rc, call);
    return;
  }
  ReplayDraw(rc, call, off_w, off_h);
}

}  // namespace
}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void DrawCapturedScene(GraphicsSystem& gfx, plume::RenderCommandList* list, plume::RenderTexture* target, const plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height, uint32_t eyes) {
  DrawFrame& frame = g_submitting;
  if (!g_frameExchange.take(frame)) {
    return;
  }
  g_framesTaken.fetch_add(1, std::memory_order_relaxed);
  gfx.NotifyCapturedFrameTaken();
  SubmittedFrameReset frame_reset;
  if (frame.draws.empty()) {
    return;
  }
  g_profileDraws = FrameProfileDetailed();
  {
    ScopedNs timed(&g_frameProfile.ns_vertices);
    ResolveDeferredVertexSources(frame);
  }

  GraphicsSystem::DrawPathContext ctx;
  if (!gfx.GetDrawPathContext(&ctx)) {
    return;
  }
  // A map failure that never clears usually means the device is gone, and plume's D3D12Buffer::map dereferences the null result, so replay stops here with the reason logged.
  if (CheckDeviceRemoved(ctx.device)) {
    return;
  }
  if (!BeginFrameRings(ctx)) {
    return;
  }
  {
    ScopedNs timed(&g_frameProfile.ns_constants);
    MaterializeConstants(frame);
  }

  EnsurePlaceholderTextures(ctx, list);

  // Before any draw of this frame is recorded; what it evicts goes to the graveyard stamped with this frame and is freed once no list in flight can name it.
  ++g_drawFrame;
  ApplyTextureInvalidations(ctx);
  ApplyTexturePackChanges(ctx);
  EvictColdTextures(ctx);
  if ((g_drawFrame % 240) == 0) SweepResidentGeometry(ctx);
  g_residentCreatesLeft = kResidentCreatesPerFrame;
  g_replacementUploadLeft = kReplacementUploadBytesPerFrame;

  ResetVulkanOcclusionPool(ctx, list);
  BindSceneDescriptors(list, ctx);

  if (frame.guest_base == nullptr) {
    return;
  }

  BeginResolveBlitFrame(ctx.frame_slot);
  ClearPendingOcclusion(ctx.frame_slot);
  // The HUD boxes come from the captured draws alone, so both eyes share them.
  std::vector<std::array<float, 4>> hud_boxes;
  std::vector<uint8_t> hud_boxed;
  ClusterHudBoxes(frame, hud_boxes, hud_boxed);
  const bool book_pick_wanted = BeginBookPickFrame();

  // STEREO: stereo replays the whole frame once per eye (`eyes` is 1 otherwise): the vertex shaders shift each eye's picture by the shared constants, and the first eye is set aside before the second starts from a cleared scene. Every pass is replayed again, since an offscreen one may depend on the view; the occlusion queries and the book pick are answered once, from the first eye.
  for (uint32_t eye = 0; eye < std::max(eyes, 1u); ++eye) {
    g_sceneEye = eye;
    if (eye > 0) gfx.StashStereoEye(list, target, framebuffer, width, height);
    ReplayContext rc(gfx, ctx, list, frame, target, framebuffer, width, height);
    if (eyes > 1) StereoEyeShift(eye, rc.stereo_shift);
    PlanSurfaces(rc);
    list->setFramebuffer(rc.current_framebuffer);
    if (eye > 0) RebindSceneLayout(rc);
    rc.hud_boxes = hud_boxes;
    rc.hud_boxed = hud_boxed;

    OpenQueriesScope open_queries(rc.ctx, rc.occlusion);
    rc.book_pick_wanted = book_pick_wanted && eye == 0;
    for (const DrawCall& call : frame.draws) {
      if (call.kind == CommandKind::QueryBegin || call.kind == CommandKind::QueryEnd) {
        if (eye == 0) ReplayOcclusionQuery(rc.ctx, list, call, rc.occlusion);
        continue;
      }
      if (call.kind != CommandKind::Draw) SuspendVulkanQuery(rc.ctx, list, rc.occlusion);
      if (call.kind == CommandKind::Bink) {
        gfx.DrawPendingBink(list, rc.current_framebuffer == framebuffer ? ctx.scene_samples : 1);
        RebindSceneLayout(rc);
        continue;
      }
      if (call.kind == CommandKind::Resolve) {
        ReplayResolveCommand(rc, call);
        continue;
      }
      ReplayCommand(rc, call);
    }

    if (rc.current_key != 0) {
      list->setFramebuffer(framebuffer);
      list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
      list->setScissors(plume::RenderRect(0, 0, width, height));
    }
    if (eye == 0) DrawBookPick(rc.ctx, list, framebuffer, width, height);
    FinishOcclusionQueries(rc.ctx, list, rc.occlusion, width, height);
  }
  g_sceneEye = 0;
  ++g_sceneFrames;
  EndFrameRings();
}

}  // namespace kameo::gpu
