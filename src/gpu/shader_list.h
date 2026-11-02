// The shader list overlay's side in the renderer: which pixel shaders the scene draws with, the textures they sample,
// and the ones switched off, whose draws are skipped. Finding which shader draws something is what it's for.
#pragma once

#include <cstdint>
#include <vector>

namespace kameo::gpu {

struct ShaderListRow {
  uint64_t hash = 0;
  // Draws in the last frame it was seen in, and how many frames ago that was.
  uint32_t draws = 0;
  uint32_t frames_ago = 0;
  bool hidden = false;
  // The first few textures it was seen sampling, by the hashes the texture dump names them with.
  std::vector<uint64_t> textures;
};

// Recording runs only while the overlay is open; switched-off shaders stay off until switched back on.
void ShaderListSetRecording(bool recording);
bool ShaderListRecording();
void ShaderListNoteDraw(uint64_t ps_hash, uint32_t frame);
void ShaderListNoteTexture(uint64_t ps_hash, uint64_t texture_hash);
bool ShaderListHides(uint64_t ps_hash);
void ShaderListSetHidden(uint64_t ps_hash, bool hidden);
void ShaderListShowAll();
std::vector<ShaderListRow> ShaderListSnapshot();

}  // namespace kameo::gpu
