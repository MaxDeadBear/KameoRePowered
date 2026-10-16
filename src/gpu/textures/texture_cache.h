#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include <plume_render_interface.h>

#include "gpu/bindless.h"
#include "gpu/graphics_system.h"
#include "gpu/textures/texture_pack.h"

namespace kameo::gpu::detail {

struct CachedTexture {
  uint32_t guest_address = 0;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderBuffer> upload;
  uint32_t heap_index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // Decides which of XenosRecomp's descriptor ranges the index is written to.
  bool volume = false;
  // The guest resolves colour and depth into one address (F018F000: src0 14 times, src4 87 times), and a copy between their formats removes the device.
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  // Only resolve targets carry one: a scaling blit renders INTO them.
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  // False until a resolve has put pixels in it; the entry is created before the copy, which can then be skipped.
  bool written = false;
  // The frame it was last drawn with, the only evidence that a guest texture has died; resolve targets are exempt.
  uint64_t last_used_frame = 0;
  bool evictable = true;
  // Hash of the guest texels that names this texture's dump and its replacement image.
  uint64_t content_hash = 0;
  ReplacementChannels replacement_channels = ReplacementChannels::Rgba;
  bool replaceable = false;
  bool replacement_pending = false;
  // Set when an edit to the replace/ folder queued this swap, so the swap is reported.
  bool replacement_live = false;
  uint64_t replacement_bytes = 0;
};

extern std::unordered_map<uint64_t, CachedTexture> g_textures;
// Evicted and replaced textures a frame in flight may still sample; retiring one frees its heap slot.
extern DeferredDestroyQueue<CachedTexture> g_textureGraveyard;
// The replay frame counter, bumped before each frame's first draw is recorded.
extern uint64_t g_drawFrame;
extern bool g_heapExhausted;

extern uint32_t g_emptyTextures;                      // uploaded but all-zero

void EvictColdTextures(GraphicsSystem::DrawPathContext& ctx);
// Drops cached uploads and resolves at guest addresses the asset and resource hooks reported reused or freed.
void ApplyTextureInvalidations(const GraphicsSystem::DrawPathContext& ctx);
void ResetTextureInvalidations();

}  // namespace kameo::gpu::detail

namespace kameo::gpu {

// Called from the texInitTexture and D3D::DestroyResource hooks.
void CaptureTextureInitialized(uint8_t* base, uint32_t asset);
void CaptureTextureDestroyed(uint8_t* base, uint32_t resource);

}  // namespace kameo::gpu
