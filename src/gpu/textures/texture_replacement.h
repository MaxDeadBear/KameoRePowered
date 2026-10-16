#pragma once

#include <cstdint>

#include "gpu/graphics_system.h"
#include "gpu/textures/texture_cache.h"

namespace kameo::gpu::detail {

// Replacement images upload inside the recorded frame, so a burst of them is spread over frames.
inline constexpr int64_t kReplacementUploadBytesPerFrame = 64ll << 20;
extern int64_t g_replacementUploadLeft;
extern uint64_t g_replacementBytes;
extern uint32_t g_replacementsLive;
extern uint64_t g_nsReplacementUpload;
// Why uploads were not eligible for dumping or replacement, for the frame report.
struct PackSkips { uint32_t shape = 0, unnormalized = 0, signed_texels = 0, format = 0, empty = 0; };
extern PackSkips g_packSkips;

// Swaps a pending replacement in once its image has decoded, retiring the original upload behind its old heap slot.
void ApplyReadyReplacement(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, CachedTexture& cached);
// Follows edits to the replace/ folder: a changed image queues a fresh swap, and a removed one drops the cached upload so the next draw rebuilds the original.
void ApplyTexturePackChanges(GraphicsSystem::DrawPathContext& ctx);

}  // namespace kameo::gpu::detail
