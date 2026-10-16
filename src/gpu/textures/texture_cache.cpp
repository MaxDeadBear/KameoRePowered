#include "gpu/textures/texture_cache.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "gpu/guest_memory.h"
#include "gpu/log.h"
#include "gpu/resolve.h"
#include "gpu/textures/texture_heap.h"

namespace kameo::gpu::detail {

std::unordered_map<uint64_t, CachedTexture> g_textures;
DeferredDestroyQueue<CachedTexture> g_textureGraveyard;
uint64_t g_drawFrame = 0;
bool g_heapExhausted = false;
uint32_t g_emptyTextures = 0;

namespace {

// Guest addresses the asset and resource hooks saw reused or freed, drained once per recorded frame.
std::mutex g_textureInvalidationMutex;
std::set<uint32_t> g_textureInvalidations;
uint64_t g_textureInitializations = 0;
uint64_t g_textureDestructions = 0;
uint32_t g_evictedTotal = 0;

}  // namespace

// Frees the coldest cached textures once the heap passes a high-water mark; nothing tells us a guest texture died, so not drawn for a few frames is the only signal, and a wrong guess just uploads it again.
void EvictColdTextures(GraphicsSystem::DrawPathContext& ctx) {
  const uint32_t cap = g_textureSlots.capacity();
  if (cap == 0) {
    return;
  }
  const uint32_t high = cap - cap / 4;
  const uint32_t low = cap / 2;
  if (g_textureSlots.live() < high) {
    return;
  }

  constexpr uint64_t kKeepFrames = 4;
  std::vector<std::pair<uint64_t, uint64_t>> cold;
  cold.reserve(g_textures.size());
  for (auto& item : g_textures) {
    if (!item.second.evictable || g_drawFrame - item.second.last_used_frame <= kKeepFrames) {
      continue;
    }
    cold.emplace_back(item.second.last_used_frame, item.first);
  }
  std::sort(cold.begin(), cold.end());

  uint32_t freed = 0;
  for (const auto& candidate : cold) {
    if (g_textureSlots.live() - freed <= low) {
      break;
    }
    auto it = g_textures.find(candidate.second);
    if (it == g_textures.end()) {
      continue;
    }
    g_textureGraveyard.queue(std::move(it->second), ctx.frame_index);
    g_textures.erase(it);
    ++freed;
  }

  if (freed) {
    g_evictedTotal += freed;
    g_heapExhausted = false;
    KGFX_INFO("evicted {} cold textures ({} live of {}, {} evicted this session)",
                freed, g_textureSlots.live(), cap, g_evictedTotal);
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      KGFX_WARN("texture heap at {}/{} and nothing is cold enough to evict; every "
                  "cached texture was drawn with in the last {} frames",
                  g_textureSlots.live(), cap, kKeepFrames);
    }
  }
}

void ApplyTextureInvalidations(const GraphicsSystem::DrawPathContext& ctx) {
  std::set<uint32_t> addresses;
  uint64_t initialized, destroyed;
  {
    std::lock_guard lock(g_textureInvalidationMutex);
    addresses.swap(g_textureInvalidations);
    initialized = g_textureInitializations;
    destroyed = g_textureDestructions;
  }
  if (addresses.empty()) return;
  uint32_t textures = 0, resolves = 0;
  for (auto it = g_textures.begin(); it != g_textures.end();) {
    if (!addresses.contains(it->second.guest_address)) { ++it; continue; }
    g_textureGraveyard.queue(std::move(it->second), ctx.frame_index);
    it = g_textures.erase(it);
    ++textures;
  }
  for (auto it = g_resolveTargets.begin(); it != g_resolveTargets.end();) {
    if (!addresses.contains(it->first.address)) { ++it; continue; }
    g_textureGraveyard.queue(std::move(it->second), ctx.frame_index);
    it = g_resolveTargets.erase(it);
    ++resolves;
  }
  if (textures || resolves) {
    KGFX_INFO("texture lifetime: invalidated {} uploads and {} resolves at {} reused/freed addresses ({} asset textures initialized, {} owned textures destroyed)", textures, resolves, addresses.size(), initialized, destroyed);
  }
}

void ResetTextureInvalidations() {
  std::lock_guard lock(g_textureInvalidationMutex);
  g_textureInvalidations.clear();
  g_textureInitializations = 0;
  g_textureDestructions = 0;
}

}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void CaptureTextureInitialized(uint8_t* base, uint32_t asset) {
  if (!asset || !GuestRangeReadable(GuestData(base, asset), 56)) return;
  const uint32_t kind = Load32(base, asset + 28);
  const uint32_t headers = Load32(base, asset + 48);
  if (!headers || (kind != 0 && kind != 2 && kind != 3 && kind != 4 && kind != 5)) return;
  const uint32_t count = kind == 4 ? Load32(base, asset + 52) : 1;
  if (!count || count > 1024) return;
  if (kind == 4 && !GuestRangeReadable(GuestData(base, headers), size_t(count) * 4)) return;
  std::lock_guard lock(g_textureInvalidationMutex);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t texture = kind == 4 ? Load32(base, headers + 4 * i) : headers;
    if (!texture || !GuestRangeReadable(GuestData(base, texture), 24)) continue;
    const uint32_t address = Load32(base, texture + 20) & 0xFFFFF000u;
    if (address) {
      g_textureInvalidations.insert(address);
      ++g_textureInitializations;
    }
  }
}

void CaptureTextureDestroyed(uint8_t* base, uint32_t resource) {
  if (!resource || !GuestRangeReadable(GuestData(base, resource), 24)) return;
  const uint32_t common = Load32(base, resource);
  if ((common & 0x70000u) != 0x30000u || (common & 0x2000000u) != 0) return;
  const uint32_t address = Load32(base, resource + 20) & 0xFFFFF000u;
  if (!address) return;
  std::lock_guard lock(g_textureInvalidationMutex);
  g_textureInvalidations.insert(address);
  ++g_textureDestructions;
}

}  // namespace kameo::gpu
