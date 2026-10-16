#pragma once

#include <cstddef>
#include <cstdint>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace kameo::gpu {

inline uint64_t GeometryContentHash(const uint8_t* data, size_t bytes) {
  return data && bytes ? XXH3_64bits(data, bytes) : 0;
}

inline uint64_t MixGeometryKey(uint64_t key, uint64_t value) {
  return (key ^ value) * 1099511628211ull;
}

inline uint32_t VertexStreamCapacity(uint32_t bytes, uint32_t stride, uint32_t element_offset, uint32_t element_size) {
  if (!stride || !element_size || element_offset > bytes || element_size > bytes - element_offset) {
    return 0;
  }
  return (bytes - element_offset - element_size) / stride + 1;
}

}  // namespace kameo::gpu
