#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {

// A per-frame upload ring for vertices, indices or constant blocks; each write returns an absolute offset into the buffer, or kNoData when the slice is full.
struct UploadRing {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t* mapped = nullptr;
  uint32_t capacity = 0;
  // The buffer is cut into kFramesInFlight slices and each frame writes only its own, since presenting no longer waits for the GPU; offsets are absolute into the buffer.
  uint32_t slice = 0;       // bytes per frame in flight
  uint32_t base = 0;        // start of the slice this frame is writing
  uint32_t used = 0;        // absolute write cursor, never below `base`
  uint32_t exhausted = 0;   // draws dropped this frame for want of room
  uint32_t peak = 0;        // high-water mark WITHIN a slice, so sizes can be tuned
  const char* name = "";
  // Source identity -> {ring offset, bytes}, so a block the capture already deduplicated is uploaded once a frame; open addressing stamped with the frame, so it neither allocates nor needs clearing.
  struct Uploaded {
    uint64_t key = 0;
    uint32_t offset = 0;
    uint32_t bytes = 0;
    uint32_t generation = 0;
  };
  static constexpr uint32_t kUploadedSlots = 1u << 15;
  static constexpr uint32_t kUploadedProbes = 16;
  std::vector<Uploaded> uploaded = std::vector<Uploaded>(kUploadedSlots);
  uint32_t generation = 0;
  // The slot holding `key` this frame, else the first slot free this frame, else nullptr when the probe window is full.
  Uploaded* find_uploaded(uint64_t key) {
    const uint32_t home = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 49);
    for (uint32_t probe = 0; probe < kUploadedProbes; ++probe) {
      Uploaded& slot = uploaded[(home + probe) & (kUploadedSlots - 1)];
      if (slot.generation != generation || slot.key == key) return &slot;
    }
    return nullptr;
  }

  bool ensure(plume::RenderDevice* device, uint32_t bytes, plume::RenderBufferFlags flags) {
    if (buffer && capacity >= bytes) {
      return true;
    }
    unmap();
    buffer.reset();
    capacity = std::max(bytes, capacity ? capacity * 2 : 4u << 20);
    slice = capacity / kFramesInFlight;
    buffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(capacity, flags));
    return buffer != nullptr;
  }

  uint32_t map_failures = 0;  // frames this ring could not be mapped at all

  void begin(uint32_t slot) {
    peak = std::max(peak, used - base);
    base = slot * slice;
    used = base;
    exhausted = 0;
    // Keyed on arena offsets, which only mean anything within one frame.
    ++generation;
    if (buffer && !mapped) {
      mapped = static_cast<uint8_t*>(buffer->map());
      if (!mapped) {
        // Every write this frame will now fail, and `write` returns kNoData for full and unmapped alike, so say so.
        ++map_failures;
        KGFX_ERROR("{} ring FAILED TO MAP ({} KB); every draw this frame will be "
                     "dropped for want of space",
                     name, capacity >> 10);
      }
    }
  }

  // Uploads a block identified by its source, reusing this frame's ring copy; the size is part of the identity, or a windowed copy is handed to a whole-buffer draw and the view runs off the ring (device removed at ExecuteCommandLists).
  uint32_t writeOnce(uint64_t source_key, const void* data, uint32_t bytes, uint32_t align) {
    if (data == nullptr) {
      return kNoData;
    }
    Uploaded* slot = find_uploaded(source_key);
    if (slot && slot->generation == generation && slot->bytes == bytes) {
      return slot->offset;
    }
    const uint32_t offset = write(data, bytes, align);
    if (offset != kNoData && slot) {
      *slot = {source_key, offset, bytes, generation};
    }
    return offset;
  }

  void unmap() {
    if (buffer && mapped) {
      buffer->unmap();
      mapped = nullptr;
    }
  }

  // Returns kNoData when the ring is full; the caller drops the draw rather than growing mid-frame, which would invalidate offsets already recorded.
  uint32_t write(const void* data, uint32_t bytes, uint32_t align) {
    if (!mapped) {
      return kNoData;  // not mapped -- see begin(), which logs it
    }
    const uint32_t offset = (used + align - 1) & ~(align - 1);
    if (offset + bytes > base + slice) {
      ++exhausted;
      return kNoData;
    }
    std::memcpy(mapped + offset, data, bytes);
    used = offset + bytes;
    return offset;
  }
};

extern UploadRing g_vertexRing;
extern UploadRing g_indexRing;
extern UploadRing g_constantRing;

// Cached vertex and index blobs never change once built, so each gets one GPU copy instead of being re-copied into the rings every frame.
extern uint64_t g_residentBytes;
extern uint32_t g_residentCreated;
// Creating one costs ~20 us, so a newly entered area spreads them over frames and the rest go through the ring meanwhile.
inline constexpr uint32_t kResidentCreatesPerFrame = 128;
extern uint32_t g_residentCreatesLeft;

plume::RenderBuffer* ResidentBufferFor(GraphicsSystem::DrawPathContext& ctx, std::shared_ptr<const void> owner, const void* data, uint32_t bytes, plume::RenderBufferFlags flags);
// Releases the GPU copies of blobs the capture caches have dropped; a frame still in flight may name them, hence the graveyard.
void SweepResidentGeometry(GraphicsSystem::DrawPathContext& ctx);
// Sizes, maps and starts this frame's slice of each ring; false when one could not be created.
bool BeginFrameRings(const GraphicsSystem::DrawPathContext& ctx);
void EndFrameRings();
void ReleaseFrameRings();

// Where a draw's vertices and indices live: the frame arena, or a buffer shared with a capture cache.
inline const uint8_t* VertexBytes(const DrawFrame& frame, const DrawCall& c) {
  if (c.vertex_blob) return c.vertex_blob->data();
  return c.vertex_data != kNoData ? frame.arena.data() + c.vertex_data : nullptr;
}
inline const uint8_t* IndexBytes(const DrawFrame& frame, const DrawCall& c) {
  if (c.index_blob) return reinterpret_cast<const uint8_t*>(c.index_blob->data());
  return c.index_data != kNoData ? frame.arena.data() + c.index_data : nullptr;
}
// Identity for the rings' per-frame dedup; arena offsets and shared buffers are separate key spaces, so a blob's address is tagged apart.
inline uint64_t IndexKey(const DrawCall& c) {
  if (c.index_blob) return 0x4000000000000000ull | reinterpret_cast<uintptr_t>(c.index_blob->data());
  return c.index_data;
}
inline uint64_t VertexKey(const DrawCall& c) {
  if (c.vertex_blob) return 0x8000000000000000ull | reinterpret_cast<uintptr_t>(c.vertex_blob->data());
  return c.vertex_data;
}

}  // namespace kameo::gpu::detail
