#pragma once

// Declarations for the generated shader cache (src/gfx/shader_cache.cpp).
//
// XenosRecomp emits the .cpp but NOT this header -- the consuming project
// supplies it, and the struct layout has to match the initialiser order the
// generator writes:
//
//     { hash, dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask }
//
// Regenerate the .cpp with:
//
//   python scripts/build_shader_cache.py \
//       --exe   thirdparty/XenosRecomp/build/XenosRecomp/Release/XenosRecomp.exe \
//       --image <dumped guest image>.bin \
//       --out   src/gfx/shader_cache.cpp --retries 20
//
// See SHADERS.md for how to obtain the decompressed image -- it is dumped out of
// a running build rather than decompressed from the XEX.

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t spirvOffset;
  uint32_t spirvSize;
  uint32_t specConstantsMask;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

// Named "compressed" by the generator, but the caches are only actually
// compressed when the `zstandard` Python module is installed. Without it the
// blobs are emitted verbatim and the compressed/decompressed sizes are equal --
// check them before deciding whether a decompression step is needed.
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
