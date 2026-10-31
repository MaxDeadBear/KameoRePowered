#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <plume_render_interface.h>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

extern std::atomic<uint32_t> g_pipelineFailures;

// plume ignores CreateGraphicsPipelineState's HRESULT, so a returned pipeline is only real if the backend object holds a PSO.
bool PipelineCreated(plume::RenderPipeline* pipeline);
// The cached pipeline for a draw's shaders, layout and state, built on first use; null when the backend rejected it. Safe from the render thread and the precache workers at once.
plume::RenderPipeline* EnsurePipeline(GraphicsSystem::DrawPathContext& ctx, const DrawCall& call, plume::RenderShader* vs, plume::RenderShader* ps, uint32_t samples, plume::RenderFormat color_format);
size_t PipelineCount();
void ReleasePipelines();
// Stops and joins the precache workers; call before anything they use is released.
void StopPipelinePrecache();
// Re-arms QueuePipelinePrecacheShaders and PrecacheDrawPipelines for a new device.
void ResetPipelinePrecache();

}  // namespace kameo::gpu::detail
