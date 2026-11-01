// Shader mods: .kshader packages in shader_mods/ beside the game, each holding stand-ins for some of the game's pixel
// shaders. A stand-in is the game's own translated shader with a mod's Shade function added (docs/modding.md).
#pragma once

#include <cstdint>

struct ShaderCacheEntry;
namespace plume {
struct RenderShader;
}

namespace kameo::gpu {

// A .kshader is this header, its entries, its mesh keys, then the blobs the entries point at, all little-endian.
struct ShaderModHeader {
  char magic[4];
  uint32_t version;
  uint32_t count;
  // With any, the package is a costume: its stand-ins are only for those meshes of a character's standard look, only
  // while that costume is chosen, and everything else draws as it would without it.
  uint32_t mesh_count;
  // The character the costume is for, by the model type its skins are listed under (Kameo's is 0x007183BF). Version 3
  // on; a version 2 package's costume is Kameo's.
  uint32_t model_type;
};
// The header's size before version 3 added model_type.
inline constexpr uint32_t kShaderModHeaderV2Bytes = 16;

struct ShaderModEntry {
  uint64_t game_hash;
  uint32_t dxil_offset;
  uint32_t dxil_size;
  uint32_t spirv_offset;
  uint32_t spirv_size;
};

// A mesh key is the XXH3 of the first bytes of a vertex buffer, as the file holds it.
inline constexpr uint32_t kShaderModMeshKeyBytes = 256;

// The stand-in a costume gives a draw with this pixel shader and vertex buffer, or null when no costume worn now covers
// that mesh and the draw takes the shader's usual one. Called as draws are recorded.
plume::RenderShader* ShaderModCostumeStandIn(const ShaderCacheEntry* ps, const uint8_t* vertices, uint32_t bytes);

// The seconds a mod's Time reads, wrapping hourly so it keeps its precision; 0 while no mod is loaded.
float ShaderModTime();

}  // namespace kameo::gpu
