#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <plume_render_interface.h>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"
#include "gpu/textures/texture_cache.h"

namespace kameo::gpu::detail {

// Host textures the guest resolved into, by guest address and shape: one address serves different surfaces, and keying on the address alone caused five separate bugs.
struct ResolveKey {
  uint32_t address = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool depth = false;

  bool operator==(const ResolveKey& o) const {
    return address == o.address && width == o.width && height == o.height && depth == o.depth;
  }
};

struct ResolveKeyHash {
  size_t operator()(const ResolveKey& k) const {
    uint64_t h = uint64_t(k.address) * 1099511628211ull;
    h ^= (uint64_t(k.width) << 32) | (uint64_t(k.height) << 48) | (k.depth ? 1u : 0u);
    h *= 1099511628211ull;
    return size_t(h ^ (h >> 29));
  }
};

// Checked before guest memory: once a target is resolved, the rendered result lives here and the guest-side bytes are stale.
extern std::unordered_map<ResolveKey, CachedTexture, ResolveKeyHash> g_resolveTargets;
// Shadow-map depth copies performed, and depth resolves with no depth-only target of that size.
extern uint32_t g_depthResolveDone;
extern uint32_t g_depthResolveMissed;

// Copies what has been rendered so far into a host texture the guest can sample; the caller rebinds its framebuffer.
void PerformResolve(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, plume::RenderTexture* source, plume::RenderTexture* depth_source, const DrawCall& call, uint32_t width, uint32_t height, bool source_is_scene);
void ReleaseResolveTargets();

}  // namespace kameo::gpu::detail
