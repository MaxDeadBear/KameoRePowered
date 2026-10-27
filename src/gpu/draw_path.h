#pragma once

// The draw path's public side: capture runs on the guest thread inside the draw hooks (gpu/draw), replay on the render thread (gpu/replay), and the frame handoff and cache lifetime live in gpu/draw_path.cpp.

#include <cstdint>
#include <vector>

#include "gpu/graphics_system.h"

namespace kameo::gpu {

// Called from the D3DDevice_DrawIndexedVertices / DrawVertices hooks.
void CaptureIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, int32_t base_vertex, uint32_t start_index, uint32_t index_count, uint32_t guest_caller, uint32_t guest_command, uint32_t guest_command_stream);
void CaptureDraw(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count);
// Captures a standalone D3DDevice_BeginVertices draw: state now, geometry at frame end, since the guest has not written the vertices yet and EndVertices is inlined.
void CaptureDrawBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t ring, uint32_t stride);
// Called from the D3DDevice_DrawVerticesUP hook; the vertex data arrives as a pointer rather than through a bound stream.
void CaptureDrawUP(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t data, uint32_t stride);
// Captures a draw whose vertices and 16-bit indices the guest wrote into ring memory itself, e.g. drawTube; texture_objects is a 16-slot snapshot taken while its textures were still bound.
void CaptureDrawIndexedUP(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t vertex_data, uint32_t stride, uint32_t index_count, uint32_t index_data, const uint32_t* texture_objects);

// Called from the Draw_Bink_textures hook, so the video composites in guest order rather than always underneath the scene.
void CaptureBinkMarker(uint8_t* base);
// Records a D3DDevice_ClearF: flags bits 0-3 clear colour targets, bit 4 depth; colour points at four floats or is 0.
void CaptureDepthClear(uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect, uint32_t colour, float depth, uint32_t caller);
// Called from the D3DDevice_Resolve hook and recorded in frame order with the draws, because a resolve copies whatever has been rendered up to that point.
void CaptureResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t source_rect, uint32_t dest_texture, uint32_t dest_point, uint32_t clear_color, float clear_depth, uint32_t caller);

// Which guest pass is executing, as its entry address (0 = none), stamped onto every draw captured inside it; thread-local because the render thread is not the only one that draws.
extern thread_local uint32_t g_guestPass;
// The text window being drawn, so its box and every line it prints are placed on screen as one HUD element.
extern thread_local uint32_t g_hudGroup;

// Called from the Swap hook: hands the frame just captured to the presenter, starts a new one, and returns how many commands the published frame holds.
uint32_t SubmitCapturedFrame(std::vector<uint32_t>* guest_reads);
// Called from PresentClear, on the presenting thread; STEREO: `eyes` is 2 for a stereo frame, which replays the scene once per eye with the first set aside in between (see gpu/stereo/stereo.h).
void DrawCapturedScene(GraphicsSystem& gfx, plume::RenderCommandList* list, plume::RenderTexture* target, const plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height, uint32_t eyes);

// True while a published captured frame is waiting for the render thread to take it.
bool CapturedFrameWaiting();
// How many captured frames the render thread has taken so far.
uint64_t CapturedFramesTaken();
// Drops a published frame nothing took, counting it as dropped.
void DiscardCapturedFrame();

// Drops the texture, sampler and pipeline caches; must run before the device goes away.
void ReleaseDrawPathCaches();
// Frees resources evicted long enough ago that no command list in flight can reference them; called at the top of each frame once its slot fence has been waited.
void RetireDrawPathGarbage(uint64_t frame_index);

}  // namespace kameo::gpu
