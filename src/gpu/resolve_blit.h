#pragma once

#include <cstdint>

#include <plume_render_interface.h>

#include "gpu/graphics_system.h"
#include "gpu/textures/texture_cache.h"

namespace kameo::gpu::detail {

// Points the blit and depth-resolve slot allocators at this frame's slice.
void BeginResolveBlitFrame(uint32_t frame_slot);
// Draws a source region given in normalised UVs into a rectangle of a resolve target; false when it could not, so the caller copies instead.
bool BlitScaledResolve(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, plume::RenderTexture* source, CachedTexture& dest, const float* uv, int32_t dest_x, int32_t dest_y, uint32_t dest_width, uint32_t dest_height);
// Draws a depth buffer's rectangle at an origin into an R32F resolve target.
bool DrawDepthResolve(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, plume::RenderTexture* depth, CachedTexture& dest, int32_t origin_x, int32_t origin_y, bool multisampled);
void ReleaseResolveBlits();

}  // namespace kameo::gpu::detail
