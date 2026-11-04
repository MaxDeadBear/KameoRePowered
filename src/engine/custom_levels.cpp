// Custom levels.
//
// The game loads any level by name: LevelChange(name) reads the archive named by the name's ELF hash (databaseMakeLevelFilename) and looks the
// level object up in it by the same hash, so a new level is an archive whose level object has its name. custom_levels.toml in the game data folder
// lists them:
//   [[level]]
//   name = "CustomTest"     # 0B660004.lvl
//   title = "Custom Test"
//   start = 0               # its PlrSetupPoint
// The game's level titles come from levelLookup, which a new level isn't in ("Invalid level name"); custom levels get string ids of their own.

#include "engine/custom_levels.h"

#include <cstdio>

#include <toml++/toml.hpp>

#include <rex/logging.h>

#include "engine/guest_addresses.h"

namespace kameo::engine {
namespace {

constexpr const char* kListFile = "custom_levels.toml";
// String ids past the game's own table, after the shader costumes' (35100) and the mod costumes' (35300).
constexpr uint32_t kCustomLevelString = 35500;

std::vector<CustomLevel> g_levels;

uint32_t ElfHash(const std::string& text) {
  uint32_t hash = 0;
  for (const char c : text) {
    hash = (hash << 4) + uint32_t(int8_t(c));
    if (const uint32_t top = hash & 0xF0000000u) hash ^= (top >> 24) | top;
  }
  return hash;
}

std::u16string Utf16(const std::string& text) {
  std::u16string out;
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i]);
    uint32_t code = c, extra = 0;
    if (c >= 0xF0) code = c & 0x07, extra = 3;
    else if (c >= 0xE0) code = c & 0x0F, extra = 2;
    else if (c >= 0xC0) code = c & 0x1F, extra = 1;
    ++i;
    for (; extra && i < text.size(); --extra, ++i) code = (code << 6) | (uint8_t(text[i]) & 0x3F);
    if (code >= 0x10000) {
      code -= 0x10000;
      out.push_back(char16_t(0xD800 + (code >> 10)));
      out.push_back(char16_t(0xDC00 + (code & 0x3FF)));
    } else {
      out.push_back(char16_t(code));
    }
  }
  return out;
}

int CustomLevelIndex(uint32_t name_hash) {
  for (size_t i = 0; i < g_levels.size(); ++i) {
    if (ElfHash(g_levels[i].name) == name_hash) return int(i);
  }
  return -1;
}

}  // namespace

void LoadCustomLevels(const std::filesystem::path& data_root) {
  g_levels.clear();
  std::error_code ec;
  const std::filesystem::path list = data_root / kListFile;
  if (!std::filesystem::exists(list, ec)) return;
  try {
    const toml::table table = toml::parse_file(list.string());
    const toml::array* levels = table["level"].as_array();
    for (size_t i = 0; levels && i < levels->size(); ++i) {
      const toml::table* entry = levels->get(i)->as_table();
      if (!entry) continue;
      CustomLevel level;
      level.name = (*entry)["name"].value_or(std::string());
      // LevelChange's buffer holds 31 characters.
      if (level.name.empty() || level.name.size() > 31) {
        REXLOG_WARN("[kameo] custom levels: entry {} needs a name of 1 to 31 characters", i + 1);
        continue;
      }
      level.title = Utf16((*entry)["title"].value_or(level.name));
      level.start = uint32_t((*entry)["start"].value_or(int64_t(0)));
      char file[16];
      std::snprintf(file, sizeof(file), "%08X.lvl", ElfHash(level.name));
      if (!std::filesystem::exists(data_root / file, ec)) {
        REXLOG_WARN("[kameo] custom levels: {} has no archive ({} in the game data folder)", level.name, file);
        continue;
      }
      if (CustomLevelIndex(ElfHash(level.name)) >= 0) continue;
      REXLOG_INFO("[kameo] custom level {} ({}), starting at setup point {}", level.name, file, level.start);
      g_levels.push_back(std::move(level));
    }
  } catch (const toml::parse_error& error) {
    REXLOG_WARN("[kameo] custom levels: {} could not be read: {}", kListFile, error.description());
  }
}

const std::vector<CustomLevel>& CustomLevels() { return g_levels; }

std::u16string CustomLevelTitleOfString(uint32_t string_id) {
  if (string_id < kCustomLevelString || string_id - kCustomLevelString >= g_levels.size()) return {};
  return g_levels[string_id - kCustomLevelString].title;
}

}  // namespace kameo::engine

// effectsLevelNameGetLevelText(levelName) -- the string id of a level's title, from levelLookup by the name's hash; a custom level gets its own.
extern "C" REX_FUNC(sub_82445268) {
  std::string name;
  for (uint32_t i = 0; ctx.r3.u32 && i < 64; ++i) {
    const char c = char(REX_LOAD_U8(ctx.r3.u32 + i));
    if (!c) break;
    name.push_back(c);
  }
  if (const int index = kameo::engine::CustomLevelIndex(kameo::engine::ElfHash(name)); index >= 0) {
    ctx.r3.u64 = kameo::engine::kCustomLevelString + uint32_t(index);
    return;
  }
  __imp__sub_82445268(ctx, base);
}
