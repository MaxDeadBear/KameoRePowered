#include "gpu/shader_list.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace kameo::gpu {

namespace {

constexpr size_t kTexturesKept = 4;

struct Seen {
  uint32_t frame = 0;
  uint32_t draws = 0;
  uint32_t last_draws = 0;
  std::vector<uint64_t> textures;
};

std::atomic<bool> g_recording{false};
std::atomic<bool> g_anyHidden{false};
std::mutex g_mutex;
std::unordered_map<uint64_t, Seen> g_seen;
std::unordered_set<uint64_t> g_hidden;
uint32_t g_frame = 0;

}  // namespace

void ShaderListSetRecording(bool recording) { g_recording.store(recording, std::memory_order_relaxed); }

bool ShaderListRecording() { return g_recording.load(std::memory_order_relaxed); }

void ShaderListNoteDraw(uint64_t ps_hash, uint32_t frame) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_frame = std::max(g_frame, frame);
  Seen& seen = g_seen[ps_hash];
  if (seen.frame != frame) {
    seen.last_draws = seen.draws;
    seen.draws = 0;
    seen.frame = frame;
  }
  ++seen.draws;
}

void ShaderListNoteTexture(uint64_t ps_hash, uint64_t texture_hash) {
  if (!texture_hash) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<uint64_t>& textures = g_seen[ps_hash].textures;
  if (textures.size() < kTexturesKept && std::find(textures.begin(), textures.end(), texture_hash) == textures.end()) textures.push_back(texture_hash);
}

bool ShaderListHides(uint64_t ps_hash) {
  if (!g_anyHidden.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_hidden.contains(ps_hash);
}

void ShaderListSetHidden(uint64_t ps_hash, bool hidden) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (hidden) g_hidden.insert(ps_hash);
  else g_hidden.erase(ps_hash);
  g_anyHidden.store(!g_hidden.empty(), std::memory_order_relaxed);
}

void ShaderListShowAll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hidden.clear();
  g_anyHidden.store(false, std::memory_order_relaxed);
}

std::vector<ShaderListRow> ShaderListSnapshot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<ShaderListRow> rows;
  rows.reserve(g_seen.size());
  for (const auto& [hash, seen] : g_seen) {
    // The frame being drawn is still counting, so the one before it is the whole count.
    const uint32_t draws = seen.frame == g_frame ? std::max(seen.draws, seen.last_draws) : seen.draws;
    rows.push_back({hash, draws, g_frame - seen.frame, g_hidden.contains(hash), seen.textures});
  }
  // A shader switched off is never drawn again, so it would drop out of view; it stays listed while it's off.
  return rows;
}

}  // namespace kameo::gpu
