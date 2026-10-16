#pragma once

// Declarations for the generated res/shader_cache/shader_cache.cpp, which XenosRecomp emits without a header.

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t spirvOffset;
  uint32_t spirvSize;
  uint32_t specConstantsMask;
  // 2 bits per sampler slot: 0 = Texture2D, 1 = Texture3D, 2 = TextureCube. Decides both the host
  // resource type and which of XenosRecomp's descriptor ranges the index is written to.
  uint32_t textureDimensionMask;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

// The generator's "compressed" names hold verbatim blobs when zstandard was missing, so compare the two sizes first.
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
