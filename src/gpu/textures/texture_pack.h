#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, kameo_gfx_dump_textures);
REXCVAR_DECLARE(bool, kameo_gfx_texture_replacements);

namespace kameo::gpu {

struct ReplacementLevel {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

struct DecodedReplacement {
  std::vector<ReplacementLevel> levels;
  uint64_t bytes = 0;
};

enum class ReplacementChannels : uint8_t { Rgba, RedOnly, RedGreen };

struct TexturePackStats {
  uint64_t dumps_written = 0;
  uint64_t dumps_dropped = 0;
  uint64_t replacements_indexed = 0;
  uint64_t decodes_pending = 0;
  uint64_t decodes_failed = 0;
  uint64_t decode_ms = 0;
};

uint64_t TextureContentHash(const uint8_t* data, size_t bytes, uint32_t width, uint32_t height, uint32_t format, uint32_t swizzle);
void NoteTextureAssetLevel(uint32_t object, const std::string& level);
void QueueTextureDump(uint64_t hash, uint32_t object, uint32_t width, uint32_t height, uint32_t format, std::vector<uint8_t>&& pixels, uint32_t src_row_bytes, bool block_compressed, bool write_file);
bool HasTextureReplacement(uint64_t hash);
std::shared_ptr<DecodedReplacement> RequestTextureReplacement(uint64_t hash, ReplacementChannels channels);
void RescanTextureReplacements();
// Re-evaluates every indexed replacement against the enable switch, so turning it off restores the originals and on swaps them back in.
void RefreshAllTextureReplacements();
std::vector<uint64_t> TakeChangedReplacements();
// Turning dumping on after textures loaded without it: the draw path reloads the ones never written so they dump.
bool TextureWasDumped(uint64_t hash);
void RequestDumpOfLoadedTextures();
bool TakeDumpOfLoadedTexturesRequest();

}  // namespace kameo::gpu
