#pragma once

#include <cstdint>

#include <plume_render_interface.h>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

// A page draw as the scene recorded it, replayed into the pick target at the end of the frame.
struct BookPickDraw {
  plume::RenderShader* vs = nullptr;
  const DrawCall* call = nullptr;
  const plume::RenderBuffer* vertex_buffer = nullptr;
  uint32_t vertex_offset = 0;
  const plume::RenderBuffer* index_buffer = nullptr;
  uint32_t index_offset = 0;
  uint32_t vs_offset = 0;
  uint32_t ps_offset = 0;
  uint32_t shared_offset = 0;
  float viewport[6] = {};
};

// Whether the pointer is over the book this frame; clears the previous frame's page draws.
bool BeginBookPickFrame();
// A 640x480 colour resolve is the book's page texture while the book is up.
bool IsBookPageTexture(uint32_t heap_index);
bool BookPickHasRoom();
void AddBookPickDraw(const BookPickDraw& draw);
// Reads back what this frame slot copied last time, then replays this frame's page draws and copies the texel under the pointer.
void DrawBookPick(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const plume::RenderFramebuffer* scene_framebuffer, uint32_t width, uint32_t height);
void ReleaseBookPick();

}  // namespace kameo::gpu::detail

namespace kameo::gpu {

// Asks the draw path to read the storybook page coordinate under a window point (0-1 across the window) for the next ~250 ms, and returns the latest answer while it is under 150 ms old.
void RequestBookPick(float window_x, float window_y);
bool LatestBookPick(float* page_x, float* page_y, bool* hit);

}  // namespace kameo::gpu
