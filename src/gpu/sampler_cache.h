#pragma once

#include <cstddef>
#include <cstdint>

#include "gpu/graphics_system.h"
#include "gpu/guest_state.h"

namespace kameo::gpu::detail {

// The bindless sampler index for a fetch constant's sampler state; `linear_depth` filters a shadow map's depth before the shader's compare.
uint32_t EnsureSampler(GraphicsSystem::DrawPathContext& ctx, const GuestTextureFetch& fetch, bool linear_depth);
// Vulkan keeps shadow-map copies in the depth format, which a device may not filter linearly.
bool ShadowMapsFilterable(const GraphicsSystem::DrawPathContext& ctx);
void ReleaseSamplers();

}  // namespace kameo::gpu::detail
