// Shader mods: .kshader packages in shader_mods/ beside the game, each holding stand-ins for some of the game's pixel
// shaders. A stand-in is the game's own translated shader with a mod's Shade function added (docs/modding.md).
#pragma once

#include <cstdint>

struct ShaderCacheEntry;

namespace kameo::gpu {

// A .kshader is this header, its entries, its mesh keys, then the blobs the entries point at, all little-endian.
struct ShaderModHeader {
  char magic[4];
  uint32_t version;
  uint32_t count;
  // With any, the package is a Bonus costume for Kameo: the stand-ins are only for those meshes of her everyday outfit,
  // only while that costume is chosen, and everything else keeps the game's shaders.
  uint32_t mesh_count;
};

struct ShaderModEntry {
  uint64_t game_hash;
  uint32_t dxil_offset;
  uint32_t dxil_size;
  uint32_t spirv_offset;
  uint32_t spirv_size;
};

// A mesh key is the XXH3 of the first bytes of a vertex buffer, as the file holds it.
inline constexpr uint32_t kShaderModMeshKeyBytes = 256;

// Whether a draw with this pixel shader and vertex buffer should keep the game's shader, because the stand-in is for other meshes.
bool ShaderModSkipsDraw(const ShaderCacheEntry* ps, const uint8_t* vertices, uint32_t bytes);

// The seconds a mod's Time reads, wrapping hourly so it keeps its precision; 0 while no mod is loaded.
float ShaderModTime();

}  // namespace kameo::gpu
