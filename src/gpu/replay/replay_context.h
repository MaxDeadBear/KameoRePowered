#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/constant_buffers.h"
#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"
#include "gpu/occlusion.h"

namespace kameo::gpu::detail {

// Scenes recorded so far, and which eye of a stereo frame is being replayed (STEREO:, always 0 without it); offscreen targets are cleared once per eye.
extern uint32_t g_sceneFrames;
extern uint32_t g_sceneEye;
// A pass's clear stamp, unique to this frame and eye, so each eye clears its own targets while age is still counted in frames.
inline uint64_t SceneStamp() { return uint64_t(g_sceneFrames) * 2 + g_sceneEye; }
inline uint64_t FramesSince(uint64_t stamp) { return uint64_t(g_sceneFrames) - (stamp >> 1); }

// What the command list already carries; root arguments survive a pipeline bind only while the layout is unchanged, so anything that rebinds the layout resets this.
struct BoundState {
  plume::RenderPipeline* pipeline = nullptr;
  uint32_t vs_offset = kNoData;
  uint32_t ps_offset = kNoData;
  uint32_t shared_offset = kNoData;
  const plume::RenderBuffer* vertex_buffer = nullptr;
  uint32_t vertex_offset = kNoData;
  uint32_t vertex_bytes = 0;
  uint32_t vertex_stride = 0;
  const plume::RenderBuffer* index_buffer = nullptr;
  uint32_t index_offset = kNoData;
  uint32_t index_bytes = 0;
  float viewport[4] = {0, 0, 0, 0};
  float depth_range[2] = {0, 0};
  int32_t scissor[4] = {0, 0, 0, 0};
  bool viewport_valid = false;
  bool scissor_valid = false;
  void reset() { *this = BoundState(); }
};

// What one recorded frame's commands share.
struct ReplayContext {
  ReplayContext(GraphicsSystem& gfx, const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, DrawFrame& frame, plume::RenderTexture* target, const plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height);

  GraphicsSystem& gfx;
  GraphicsSystem::DrawPathContext ctx;
  plume::RenderCommandList* list;
  DrawFrame& frame;
  uint8_t* base;
  // The scene's own target, framebuffer and size.
  plume::RenderTexture* target;
  const plume::RenderFramebuffer* framebuffer;
  uint32_t width;
  uint32_t height;
  // The scene's guest surface, and per other surface the size its resolves name and the extent its draws reach.
  uint32_t main_surface = 0;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> surface_dims;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> surface_extent;
  // The surface the command stream is rendering into: the scene until a pass redirects it, and resolves copy from whichever is active.
  plume::RenderTexture* current_texture;
  const plume::RenderFramebuffer* current_framebuffer;
  plume::RenderFormat current_color_format;
  uint64_t current_key = 0;
  BoundState bound;
  OcclusionReplay occlusion;
  std::vector<std::array<float, 4>> hud_boxes;
  std::vector<uint8_t> hud_boxed;
  // The per-draw shared block, and the previous one with its ring offset so consecutive draws sampling the same textures reuse it.
  uint8_t shared[kSharedConstantBytes];
  uint8_t shared_prev[kSharedConstantBytes];
  uint32_t shared_prev_offset = kNoData;
  bool shared_prev_valid = false;
  // STEREO: the eye's clip-space shift (separation, convergence) for draws into the scene; zero without stereo. See gpu/stereo.h.
  float stereo_shift[2] = {0.0f, 0.0f};
  bool book_pick_wanted = false;
};

// Binds the bindless pipeline layout and its four descriptor sets.
void BindSceneDescriptors(plume::RenderCommandList* list, const GraphicsSystem::DrawPathContext& ctx);
// Rebinds them after anything that changed the layout, forgetting what was bound.
void RebindSceneLayout(ReplayContext& rc);
// Finds the scene's surface and, per other surface, the size its resolves name and the extent its draws reach.
void PlanSurfaces(ReplayContext& rc);
// Points the command list at the command's target -- the scene, an offscreen surface or a depth-only framebuffer -- and reports the offscreen size; false drops the command.
bool RouteDrawTarget(ReplayContext& rc, const DrawCall& call, uint32_t& off_w, uint32_t& off_h);
void ReplayDepthClear(ReplayContext& rc, const DrawCall& call);
// Replays a guest resolve from the surface it names, then any clear it asked for.
void ReplayResolveCommand(ReplayContext& rc, const DrawCall& call);

}  // namespace kameo::gpu::detail
