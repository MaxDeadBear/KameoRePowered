#include "gpu/draw/indices.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "gpu/draw/capture.h"
#include "gpu/frame_profile.h"
#include "gpu/guest_memory.h"
#include "gpu/guest_state.h"
#include "gpu/vertex_transcode.h"

namespace kameo::gpu::detail {

std::atomic<uint32_t> g_indexCacheHits{0};
std::atomic<uint32_t> g_indexCacheMiss{0};

namespace {

// A draw's expanded index list is a pure function of the guest index bytes, the primitive and the base vertex, which level geometry supplies unchanged every frame.
struct IndexCacheEntry {
  std::shared_ptr<const std::vector<uint32_t>> blob;
  uint32_t min_vertex = 0;
  uint32_t max_vertex = 0;
  uint64_t hash = 0;        // of the SOURCE bytes, so a restream is caught
  uint64_t last_frame = 0;
};
std::unordered_map<uint64_t, IndexCacheEntry> g_indexCache;
// Direct-mapped front for g_indexCache: its nodes never move, so a hit costs one compare instead of a bucket walk; cleared whenever entries are erased.
struct IndexCacheMemo {
  uint64_t key = 0;
  IndexCacheEntry* entry = nullptr;
};
constexpr uint32_t kIndexCacheMemoSlots = 4096;
IndexCacheMemo g_indexCacheMemo[kIndexCacheMemoSlots];

IndexCacheEntry* FindIndexCacheEntry(uint64_t key) {
  IndexCacheMemo& memo = g_indexCacheMemo[(key ^ (key >> 29)) & (kIndexCacheMemoSlots - 1)];
  if (memo.entry && memo.key == key) return memo.entry;
  auto found = g_indexCache.find(key);
  if (found == g_indexCache.end()) return nullptr;
  memo = IndexCacheMemo{key, &found->second};
  return &found->second;
}

bool IsStripPrimitive(uint32_t primitive) {
  return primitive == kPrimTriangleStrip || primitive == kPrimLineStrip;
}

}  // namespace

std::optional<uint32_t> GuestStripReset(uint8_t* base, uint32_t device, uint32_t primitive, bool index32) {
  if (!IsStripPrimitive(primitive) || (Load32(base, device + state::kModeControl) & state::kPrimitiveResetEnableBit) == 0) return std::nullopt;
  return Load32(base, device + state::kPrimitiveResetIndex) & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
}

bool ExpandTopology(uint32_t primitive, uint32_t vertex_count, const uint32_t* indices, std::vector<uint32_t>* out, uint32_t* out_min, uint32_t* out_max) {
  auto index_at = [&](uint32_t i) { return indices ? indices[i] : i; };
  uint32_t lo = ~0u;
  uint32_t hi = 0;
  auto note = [&](uint32_t v) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  };
  // Every return path reports the range; nothing reads it when the call fails or the output is empty.
  struct Report {
    uint32_t *min_out, *max_out, *lo, *hi;
    ~Report() { *min_out = *lo; *max_out = *hi; }
  } report{out_min, out_max, &lo, &hi};

  out->clear();
  switch (primitive) {
    case kPrimTriangleList:
    case kPrimLineList:
    case kPrimPointList:
      if (indices) {
        // A straight copy: resize() would value-initialise every element first.
        out->assign(indices, indices + vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i) note((*out)[i]);
      } else {
        out->reserve(vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i) out->push_back(i);
        if (vertex_count) {
          lo = 0;
          hi = vertex_count - 1;
        }
      }
      return true;

    // Strips are expanded rather than drawn as strips: plume sets IBStripCutValue to the 16-bit 0xFFFF for any strip topology while this renderer binds 32-bit indices, which D3D12 rejects, so every strip pipeline failed.
    case kPrimTriangleStrip: {
      if (vertex_count < 3) return false;
      out->reserve(size_t(vertex_count - 2) * 3);
      uint32_t run_start = 0;
      for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint32_t c = index_at(i);
        if (c == kStripRestart) {
          run_start = i + 1;  // restart: the next three indices begin a new strip
          continue;
        }
        const uint32_t n = i - run_start;
        if (n < 2) continue;
        const uint32_t a = index_at(i - 2);
        const uint32_t b = index_at(i - 1);
        // Strips stitch runs together with degenerate triangles; dropping them saves the rasteriser rejecting them.
        if (a == b || b == c || a == c) continue;
        out->push_back(a);
        out->push_back((n & 1) ? c : b);  // odd triangles are wound the other way
        out->push_back((n & 1) ? b : c);
        note(a);
        note(b);
        note(c);
      }
      return !out->empty();
    }

    case kPrimLineStrip: {
      if (vertex_count < 2) return false;
      out->reserve(size_t(vertex_count - 1) * 2);
      uint32_t run_start = 0;
      for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint32_t c = index_at(i);
        if (c == kStripRestart) {
          run_start = i + 1;
          continue;
        }
        if (i - run_start < 1) continue;
        const uint32_t p = index_at(i - 1);
        out->push_back(p);
        out->push_back(c);
        note(p);
        note(c);
      }
      return !out->empty();
    }

    case kPrimTriangleFan: {
      if (vertex_count < 3) return false;
      const uint32_t triangles = vertex_count - 2;
      out->reserve(triangles * 3);
      const uint32_t hub = index_at(0);
      note(hub);
      for (uint32_t t = 0; t < triangles; ++t) {
        const uint32_t b = index_at(t + 1);
        const uint32_t c = index_at(t + 2);
        out->push_back(hub);
        out->push_back(b);
        out->push_back(c);
        note(b);
        note(c);
      }
      return true;
    }

    case kPrimQuadList: {
      if (vertex_count < 4) return false;
      const uint32_t quads = vertex_count / 4;
      out->reserve(quads * 6);
      for (uint32_t q = 0; q < quads; ++q) {
        const uint32_t v = q * 4;
        const uint32_t a = index_at(v + 0);
        const uint32_t b = index_at(v + 1);
        const uint32_t c = index_at(v + 2);
        const uint32_t d = index_at(v + 3);
        out->push_back(a);
        out->push_back(b);
        out->push_back(c);
        out->push_back(a);
        out->push_back(c);
        out->push_back(d);
        note(a);
        note(b);
        note(c);
        note(d);
      }
      return true;
    }

    default:
      return false;
  }
}

void ReadIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, int32_t base_vertex, uint32_t address, uint32_t start_index, uint32_t index_count, bool index32, std::vector<uint32_t>* indices, ExpandedIndices* pre) {
  const uint32_t element = index32 ? 4 : 2;
  const uint8_t* src = GuestData(base, address + start_index * element);
  const size_t source_bytes = size_t(index_count) * element;
  const std::optional<uint32_t> reset = GuestStripReset(base, device, primitive, index32);

  // What fully determines the expanded list: where the indices are, how many and how wide, the base vertex folded in during the swap, the primitive and the strip restart index.
  uint64_t cache_key = 1469598103934665603ull;
  for (uint64_t v : {uint64_t(address + start_index * element), uint64_t(index_count),
                     uint64_t(index32), uint64_t(uint32_t(base_vertex)), uint64_t(primitive),
                     reset ? uint64_t(*reset) : ~uint64_t(0)}) {
    cache_key = (cache_key ^ v) * 1099511628211ull;
  }
  // Hashed once per frame like the vertex spans: the shadow, depth and colour passes each draw the same range.
  uint64_t source_hash = 0;
  {
    ScopedAtomicNs timed(g_captureDetailed ? &g_frameProfile.ns_cap_hash : nullptr);
    source_hash = FrameSpanHash(src, source_bytes, g_captureFrame);
  }

  IndexCacheEntry* cached = FindIndexCacheEntry(cache_key);
  if (cached && cached->hash == source_hash) {
    cached->last_frame = g_captureFrame;
    pre->blob = cached->blob;
    pre->min_vertex = cached->min_vertex;
    pre->max_vertex = cached->max_vertex;
    ++g_indexCacheHits;
  } else {
    ++g_indexCacheMiss;
    indices->resize(index_count);
    uint32_t* dst = indices->data();
    // The width test and base-vertex fold are loop-invariant, so they stay out of the loop and the swap vectorises.
    if (index32) {
      for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t value;
        std::memcpy(&value, src + 4 * i, 4);
        dst[i] = Swap32(value) + uint32_t(base_vertex);
      }
    } else {
      for (uint32_t i = 0; i < index_count; ++i) {
        uint16_t value;
        std::memcpy(&value, src + 2 * i, 2);
        dst[i] = uint32_t(Swap16(value)) + uint32_t(base_vertex);
      }
    }
    // The reset index is compared with the raw guest index, before the base vertex is folded in.
    if (reset) {
      for (uint32_t i = 0; i < index_count; ++i) {
        if (dst[i] - uint32_t(base_vertex) == *reset) dst[i] = kStripRestart;
      }
    }

    // Expand once and keep it, in a new buffer every time: a published frame may still be drawing from the old one.
    auto blob = std::make_shared<std::vector<uint32_t>>();
    uint32_t lo = 0, hi = 0;
    if (ExpandTopology(primitive, index_count, dst, blob.get(), &lo, &hi) && !blob->empty()) {
      IndexCacheEntry& entry = g_indexCache[cache_key];
      entry.blob = blob;
      entry.min_vertex = lo;
      entry.max_vertex = hi;
      entry.hash = source_hash;
      entry.last_frame = g_captureFrame;
      pre->blob = std::move(blob);
      pre->min_vertex = lo;
      pre->max_vertex = hi;
    }
  }
}

size_t EvictIndexCache(uint64_t frame) {
  // Erasing only drops the map's reference; any frame still holding the buffer keeps it alive.
  std::fill(std::begin(g_indexCacheMemo), std::end(g_indexCacheMemo), IndexCacheMemo{});
  size_t live = 0;
  for (auto it = g_indexCache.begin(); it != g_indexCache.end();) {
    if (frame - it->second.last_frame > 240) {
      it = g_indexCache.erase(it);
    } else {
      live += it->second.blob ? it->second.blob->size() * 4 : 0;
      ++it;
    }
  }
  return live;
}

void ReleaseIndexCache() {
  std::fill(std::begin(g_indexCacheMemo), std::end(g_indexCacheMemo), IndexCacheMemo{});
  g_indexCache.clear();
}

}  // namespace kameo::gpu::detail
