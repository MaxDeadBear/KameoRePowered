#pragma once

#include <cstdint>

#include "gpu/graphics_system.h"
#include "gpu/guest_state.h"

namespace kameo::gpu::detail {

// Mip statistics for the frame report.
extern uint32_t g_mipLevelsUploaded;
extern uint32_t g_mippedTextures;
extern uint32_t g_mipChainsTruncated;

// Untiles, converts and uploads one guest texture, returning its bindless heap index (0, the fallback, on failure).
// Bumped whenever a resolve writes a target: a texture sampled before and after that resolve in one frame answers differently, so EnsureTexture's per-frame memo must not carry the earlier answer across it.
extern uint32_t g_textureMemoGeneration;

uint32_t EnsureTexture(uint8_t* base, GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const GuestTextureFetch& fetch, uint32_t object, uint32_t object_address, uint32_t want_dimension, bool* volume_out);

}  // namespace kameo::gpu::detail
