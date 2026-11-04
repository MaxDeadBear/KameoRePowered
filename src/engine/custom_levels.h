// Custom levels: new .lvl archives in the game data folder, listed in its custom_levels.toml, shown in Level Select and titled
// as the game titles its own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kameo::engine {

struct CustomLevel {
  std::string name;      // what LevelChange is given; the file is the ELF hash of it in hex, .lvl
  std::u16string title;  // shown on arrival and in Level Select
  uint32_t start = 0;    // the PlrSetupPoint the player starts at
};

// Reads <data root>/custom_levels.toml, keeping the levels whose archive is there.
void LoadCustomLevels(const std::filesystem::path& data_root);
const std::vector<CustomLevel>& CustomLevels();
// The title of a custom level's string id, which effectsLevelNameGetLevelText hands out for it; empty for other ids.
std::u16string CustomLevelTitleOfString(uint32_t string_id);

}  // namespace kameo::engine
