#pragma once

#include <atomic>
#include <cstdint>

#include <plume_render_interface.h>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

// The query the guest has open; a null-pixel-shader draw inside one is its visibility test quad.
extern std::atomic<uint32_t> g_openOcclusionQuery;

inline constexpr uint32_t kOcclusionQueriesPerFrame = 1024;

struct OcclusionReplay {
  uint32_t used = 0;
  uint32_t active_query = 0;
  uint32_t active_index = 0;
  bool active = false;
  // A Vulkan query closed early because its render pass had to end; its own end still reports what it counted.
  uint32_t closed_query = 0;
  uint32_t closed_index = 0;
};

// A Vulkan query has to be reset outside a render pass before it can begin again.
void ResetVulkanOcclusionPool(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list);
void ClearPendingOcclusion(uint32_t frame_slot);
void ReplayOcclusionQuery(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const DrawCall& call, OcclusionReplay& replay);
// A Vulkan query may not outlive the render pass it began in, so one still open is ended before anything that ends the pass.
void SuspendVulkanQuery(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, OcclusionReplay& replay);
// Ends the draw loop's open Vulkan query, so a texture upload recorded mid-draw can place its barrier.
void SuspendOpenQuery(plume::RenderCommandList* list);
// Publishes the draw loop's queries to SuspendOpenQuery for the scope of a frame.
struct OpenQueriesScope {
  OpenQueriesScope(const GraphicsSystem::DrawPathContext& ctx, OcclusionReplay& replay);
  ~OpenQueriesScope();
};
void FinishOcclusionQueries(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, OcclusionReplay& replay, uint32_t width, uint32_t height);
void ReleaseOcclusion();

}  // namespace kameo::gpu::detail

namespace kameo::gpu {

// Records a D3DQuery_Issue BEGIN or END of an occlusion query so the replay counts the samples its draws pass.
void CaptureOcclusionQuery(uint8_t* base, uint32_t query, bool begin);
// False while the query's latest END is still on the GPU, otherwise its sample count at guest resolution.
bool OcclusionQueryResult(uint32_t query, uint32_t* samples);
// Hands the occlusion counts a retired frame slot resolved back to the guest's GetData.
void PublishOcclusionResults(uint32_t frame_slot, bool retired);

}  // namespace kameo::gpu
