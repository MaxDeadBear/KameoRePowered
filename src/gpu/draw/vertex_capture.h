#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

// DrawVerticesUP hands its vertices over as a pointer, so stream 0 is overridden rather than resolved from the device.
struct StreamOverride {
  const uint8_t* data = nullptr;
  uint32_t stride = 0;
  uint32_t count = 0;
  bool active() const { return data != nullptr; }
};

// Transcodes vertices covering at least [min_vertex, max_vertex], preferring the whole source buffer; `out_base` is what the caller subtracts from each index.
bool CaptureVertices(uint8_t* base, uint32_t device, const HostLayout& layout, uint32_t min_vertex, uint32_t max_vertex, DrawFrame* frame, uint32_t* out_offset, uint32_t* out_bytes, uint32_t* out_base, uint32_t* out_count, std::shared_ptr<const std::vector<uint8_t>>* out_blob, const float* viewport, const StreamOverride& override_stream = {}, uint32_t* out_source = nullptr);
// Drops cross-frame transcodes not drawn for 240 frames, returning the bytes still cached.
size_t EvictTranscodeCache(uint64_t frame);
void ClearVertexReuse();
void ReleaseVertexCaptureCaches();
// Reads, hashes and transcodes every deferred vertex window of the frame before anything reads its vertices; the guest keeps those buffers fenced until the frame is recorded.
void ResolveDeferredVertexSources(DrawFrame& frame);

}  // namespace kameo::gpu::detail
