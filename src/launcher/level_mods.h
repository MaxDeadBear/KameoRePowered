// Level mod packages (.kmod) from the mods folder, and the data folder the game runs from when some are enabled.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kameo::launcher {

struct LevelMod {
  std::filesystem::path file;
  std::string name, author, version, description, error;
  std::vector<std::string> levels;
  bool enabled = false;
};

class LevelMods {
 public:
  void Load(const std::filesystem::path& launcher_folder);
  void Save() const;
  std::vector<LevelMod>& mods() { return mods_; }
  const std::filesystem::path& folder() const { return folder_; }
  const std::filesystem::path& assets() const { return assets_; }
  bool AnyEnabled() const;
  std::vector<std::string> Conflicts() const;
  void Move(size_t index, int step);
  bool Build(std::filesystem::path& data_root, std::string& error);

 private:
  std::filesystem::path launcher_folder_, folder_, assets_;
  std::vector<LevelMod> mods_;
};

}  // namespace kameo::launcher
