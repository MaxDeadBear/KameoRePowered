#pragma once

#include <cstdint>

#include <plume_render_interface.h>

namespace kameo::gpu::detail {

plume::RenderPrimitiveTopology HostTopology(uint32_t primitive);
plume::RenderComparisonFunction HostCompare(uint32_t func);
// Xenos stencil ops run KEEP, ZERO, REPLACE, INCR_CLAMP, DECR_CLAMP, INVERT, INCR_WRAP, DECR_WRAP -- plume's order after its UNKNOWN.
plume::RenderStencilFaceDesc HostStencilFace(uint32_t depth_control, uint32_t shift);
plume::RenderBlend HostBlend(uint32_t factor);
plume::RenderBlend HostBlendAlpha(uint32_t factor);
plume::RenderBlendOperation HostBlendOp(uint32_t op);

}  // namespace kameo::gpu::detail
