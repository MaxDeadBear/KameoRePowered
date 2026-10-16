#include "gpu/draw_path.h"

#include <mutex>

#include "gpu/constant_buffers.h"
#include "gpu/draw/capture.h"
#include "gpu/draw/draw_merge.h"
#include "gpu/draw/indices.h"
#include "gpu/draw/vertex_capture.h"
#include "gpu/frame_ring.h"
#include "gpu/occlusion.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/replay/book_pick.h"
#include "gpu/resolve.h"
#include "gpu/resolve_blit.h"
#include "gpu/sampler_cache.h"
#include "gpu/shaders/embedded_shaders.h"
#include "gpu/surface_pool.h"
#include "gpu/textures/texture_cache.h"
#include "gpu/textures/texture_heap.h"
#include "gpu/textures/texture_replacement.h"

// The draw path's lifetime across frames and devices; capture lives in gpu/draw, replay in gpu/replay.
namespace kameo::gpu {
using namespace detail;

bool CapturedFrameWaiting() { return g_frameExchange.waiting(); }

uint64_t CapturedFramesTaken() { return g_framesTaken.load(std::memory_order_relaxed); }

void DiscardCapturedFrame() {
  static DrawFrame discarded;
  if (g_frameExchange.take(discarded)) {
    discarded.reset();
  }
}

void RetireDrawPathGarbage(uint64_t frame_index) {
  g_textureGraveyard.retire(frame_index, kFramesInFlight, [](CachedTexture& texture) {
    g_textureSlots.free(texture.heap_index);
    if (texture.replacement_bytes) {
      g_replacementBytes -= texture.replacement_bytes;
      --g_replacementsLive;
    }
  });
  g_offscreenGraveyard.retire(frame_index, kFramesInFlight);
  g_uploadGraveyard.retire(frame_index, kFramesInFlight);
}

void ReleaseDrawPathCaches() {
  ResetTextureInvalidations();
  {
    std::lock_guard lock(g_captureMutex);
    g_frameExchange.clear();
    ResetDrawMerges();
    g_capturing = DrawFrame{};
    g_submitting = DrawFrame{};
    ReleaseVertexCaptureCaches();
    ReleaseIndexCache();
    g_vsDedup.reset();
    g_psDedup.reset();
  }
  ReleaseFrameRings();
  ReleasePipelines();
  ReleaseEmbeddedShaders();
  ReleaseOcclusion();
  ResetPipelinePrecache();
  ReleaseSamplers();
  g_textures.clear();
  ReleaseResolveTargets();
  g_offscreen.clear();
  g_depthOnly.clear();
  ReleasePlaceholderTextures();
  g_textureGraveyard.drain();
  g_offscreenGraveyard.drain();
  g_uploadGraveyard.drain();
  g_textureSlots.reset(0, 1);
  g_heapExhausted = false;
  ReleaseResolveBlits();
  ReleaseBookPick();
}

}  // namespace kameo::gpu
