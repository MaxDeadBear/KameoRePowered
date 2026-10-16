#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace kameo::gpu::detail {

// An already-expanded index list from the index cache, so the capture skips the expansion; empty on paths with no cache to draw on.
struct ExpandedIndices {
  std::shared_ptr<const std::vector<uint32_t>> blob;
  uint32_t min_vertex = 0;
  uint32_t max_vertex = 0;
  bool valid() const { return blob != nullptr; }
};

// Written in place of a guest index equal to the primitive reset index, and only while the guest has reset enabled.
inline constexpr uint32_t kStripRestart = 0xFFFFFFFFu;

extern std::atomic<uint32_t> g_indexCacheHits;
extern std::atomic<uint32_t> g_indexCacheMiss;

// The guest's primitive reset index for a strip draw, or nothing when reset is off: 0xFFFF is an ordinary vertex in a 32-bit index buffer.
std::optional<uint32_t> GuestStripReset(uint8_t* base, uint32_t device, uint32_t primitive, bool index32);
// Expands fans, quads and strips into lists, returning false for a primitive that is not drawn and reporting the index range emitted.
bool ExpandTopology(uint32_t primitive, uint32_t vertex_count, const uint32_t* indices, std::vector<uint32_t>* out, uint32_t* out_min, uint32_t* out_max);
// Reads and expands an indexed draw's guest indices through the cross-frame index cache; `pre` stays empty when the expansion fails, leaving the swapped list in `indices`.
void ReadIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, int32_t base_vertex, uint32_t address, uint32_t start_index, uint32_t index_count, bool index32, std::vector<uint32_t>* indices, ExpandedIndices* pre);
// Drops cached lists not drawn for 240 frames, returning the bytes still cached.
size_t EvictIndexCache(uint64_t frame);
void ReleaseIndexCache();

}  // namespace kameo::gpu::detail
