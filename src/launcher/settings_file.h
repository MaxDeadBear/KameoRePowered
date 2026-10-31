// Kameo RePowered launcher: kameo_settings.toml read and written through the same schema the game uses.

#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "core/settings_schema.h"

namespace kameo::launcher {

class Settings {
 public:
  explicit Settings(std::filesystem::path path) : path_(std::move(path)) {}

  void Load();
  bool Save();

  std::string Get(const std::string& name) const;
  bool GetBool(const std::string& name) const { return Get(name) == "true"; }
  int GetInt(const std::string& name) const;
  double GetDouble(const std::string& name) const;

  // Every change is written straight away, the way the pause menu does it.
  void Set(const std::string& name, const std::string& value);
  void SetBool(const std::string& name, bool value) { Set(name, value ? "true" : "false"); }
  void SetInt(const std::string& name, int value) { Set(name, std::to_string(value)); }
  void SetDouble(const std::string& name, double value);
  void Reset(const std::string& name);

  const std::filesystem::path& path() const { return path_; }
  const std::string& error() const { return error_; }

 private:
  std::filesystem::path path_;
  std::map<std::string, std::string> values_;
  std::string error_;
};

// The runtime's own fullscreen flag from kameorepowered.toml, which a window mode of -1 follows.
bool ReadMainConfigFullscreen(const std::filesystem::path& config_path);
// user_data_root from kameorepowered.toml, or empty when it is not set.
std::string ReadMainConfigUserDataRoot(const std::filesystem::path& config_path);
// game_data_root from kameorepowered.toml, or empty when it is not set.
std::string ReadMainConfigGameDataRoot(const std::filesystem::path& config_path);

}  // namespace kameo::launcher
