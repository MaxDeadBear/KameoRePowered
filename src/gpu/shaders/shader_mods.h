// Shader mods: .kshader packages in shader_mods/ beside the game, each holding stand-ins for some of the game's pixel
// shaders. A stand-in is the game's own translated shader with a mod's Shade function added (docs/modding.md).
#pragma once

#include <cstdint>
#include <filesystem>

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
  // Version 4 on: how many of the game's textures the mod draws with, listed after the mesh keys.
  uint32_t texture_count;
  // Version 5 on: kShaderModSeeThrough when the mod's Opacity can be below 1.
  uint32_t flags;
};
inline constexpr uint32_t kShaderModSeeThrough = 1;
// The header's size before version 3 added model_type, and before version 4 added texture_count.
inline constexpr uint32_t kShaderModHeaderV2Bytes = 16;
inline constexpr uint32_t kShaderModHeaderV3Bytes = 20;
inline constexpr uint32_t kShaderModHeaderV4Bytes = 24;

// One of the game's textures a mod draws with (its Game texture node, ModGameTexture(slot, uv) in shader_mod.hlsli):
// named, never carried, so a package holds none of the game's pictures. The game reads it from the player's own copy
// of the archive.
inline constexpr uint32_t kShaderModTextureSlots = 4;
struct ShaderModTexture {
  char archive[24];  // the file in the game's data folder, such as "024CE245.lvl"; zero-padded
  uint32_t entry;    // its "texture" entry there, 1-based
  uint32_t width;    // as the editor found it, so a different copy of the file is noticed
  uint32_t height;
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

// The game's textures one package draws with, in its slots; kept for the session once made.
struct ShaderModTextureSet;

// The stand-in a costume gives a draw with this pixel shader and vertex buffer, or null when no costume worn now covers
// that mesh and the draw takes the shader's usual one; `textures` gets that costume's textures (null for none). Called
// as draws are recorded.
plume::RenderShader* ShaderModCostumeStandIn(const ShaderCacheEntry* ps, const uint8_t* vertices, uint32_t bytes, const ShaderModTextureSet** textures = nullptr);
// The textures of the package that gives this shader its stand-in for every draw (not a costume's), or null.
const ShaderModTextureSet* ShaderModTexturesFor(const ShaderCacheEntry* ps);
// Each slot's bindless texture index, 0 where it has none or isn't uploaded yet.
void ShaderModTextureSlots(const ShaderModTextureSet* set, uint32_t out[kShaderModTextureSlots]);

// Whether a stand-in is from a see-through mod, whose solid draws are given multisample coverage to show its Opacity
// with (MOD_SHOW_* in shader_mod.hlsli).
bool ShaderModSeeThrough(const plume::RenderShader* module);
// How a draw's stand-in shows its opacity (c20.y): see_through from ShaderModSeeThrough.
uint32_t ShaderModShow(bool see_through, uint32_t blend_control, bool alpha_to_mask, bool multisampled);

// The seconds a mod's Time reads, wrapping hourly so it keeps its precision; 0 while no mod is loaded.
float ShaderModTime();

// Where the game's archives are read from for a package's textures: the game data folder.
void SetShaderModDataRoot(const std::filesystem::path& root);

}  // namespace kameo::gpu
