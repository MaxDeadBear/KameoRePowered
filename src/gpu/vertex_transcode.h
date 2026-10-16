#pragma once

#include <cstddef>
#include <cstdint>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

// Hundreds of draws a frame share one packed vertex buffer, so each source span is hashed once per frame rather than once per draw.
struct SpanHash {
  uint64_t key = 0;
  uint64_t hash = 0;
  uint64_t frame = ~0ull;
};
inline constexpr uint32_t kSpanHashSlots = 1u << 13;
// The guest thread's table, and the render thread's for the vertex sources it resolves.
extern SpanHash g_spanHashes[kSpanHashSlots];
extern SpanHash g_renderSpanHashes[kSpanHashSlots];

uint64_t FrameSpanHashIn(SpanHash* table, const uint8_t* src, size_t len, uint64_t frame);
uint64_t FrameSpanHash(const uint8_t* src, size_t len, uint64_t frame);

// A bound stream as the transcode reads it.
struct VertexStream {
  const uint8_t* data = nullptr;
  uint32_t stride = 0;
  uint32_t size = 0;  // bytes available from `data`
  uint32_t element_end = 0;
};

// Folds each stream the layout reads into the reuse key and returns the hash of the bytes the window covers.
uint64_t HashVertexWindow(const HostLayout& layout, const VertexStream* streams, uint32_t first, uint32_t count, SpanHash* table, uint64_t frame, uint64_t* reuse_key);
// Converts `count` vertices from `first` into the host layout; screen-space positions are mapped into the guest viewport's clip space.
bool TranscodeVertexWindow(const HostLayout& layout, const VertexStream* streams, uint32_t first, uint32_t count, uint8_t* dst_base, const float* viewport);

}  // namespace kameo::gpu::detail
