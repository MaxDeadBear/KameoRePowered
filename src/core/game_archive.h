// The game's CAFF archives (.lvl, .mdl) read from the player's own files: their sections unpacked, their relocated
// pointers, and their textures decoded to RGBA. The formats are described in tools/modkit/kameo_modkit (caff.py,
// graph.py, texture.py), which this reads the same way. Used by the Shader Editor's preview and by shader mods that
// draw with one of the game's textures (gpu/shaders/shader_mods.cpp), which name the texture rather than carry it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kameo::game_archive {

uint32_t Be32(const uint8_t* p);
uint16_t Be16(const uint8_t* p);

struct Ref {
  uint32_t entry = 0;  // 1-based, as the relocations name them; 0 is none
  uint32_t offset = 0;
  explicit operator bool() const { return entry != 0; }
};

class Archive {
 public:
  bool Load(const std::filesystem::path& path, std::string& error);

  size_t entry_count() const { return entries_.size(); }
  // The bytes at a reference, or null when it would run past its entry.
  const uint8_t* At(Ref ref, size_t offset, size_t bytes) const;
  uint8_t U8(Ref ref, size_t offset = 0) const;
  uint16_t U16(Ref ref, size_t offset = 0) const;
  uint32_t U32(Ref ref, size_t offset = 0) const;
  float F32(Ref ref, size_t offset = 0) const;
  Ref Pointer(Ref ref, size_t offset = 0) const;
  std::string_view String(Ref ref) const;
  // The name an entry starts with ("texture", "rendergraph", "kameoDatabase"...).
  std::string_view TypeName(uint32_t entry) const;
  // The entries an entry points into.
  std::vector<uint32_t> Targets(uint32_t entry) const;

 private:
  struct Entry {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint8_t section = 0;
  };
  std::vector<std::vector<uint8_t>> sections_;
  std::vector<Entry> entries_;
  std::unordered_map<uint64_t, uint32_t> pointers_;
  mutable std::unordered_map<uint32_t, std::vector<uint32_t>> targets_;
};

struct Texture {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};
// A texture's base level (a "texture" entry, or what a rendergraph's palette points at): untiled, swapped to little
// endian, then decoded. Null for a format it doesn't read.
std::optional<Texture> ReadTexture(const Archive& archive, Ref texture);

// The object types the archive's database defines, in its order.
std::vector<std::string> ObjectTypes(const Archive& archive);
// The "texture" entries an object type reaches through its assets (its model's colour maps and the like), in entry order.
std::vector<uint32_t> TypeTextures(const Archive& archive, std::string_view type);

}  // namespace kameo::game_archive
