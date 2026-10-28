// Kameo RePowered launcher: kameo_settings.toml read and written through the same schema the game uses.

#include "launcher/settings_file.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <system_error>

#include <toml++/toml.hpp>

namespace kameo::launcher {

namespace {

// By the node's own type: toml++'s value<bool>() also converts integers, which read every number as true.
std::string NodeToString(const toml::node& node) {
  if (node.is_boolean()) return *node.value_exact<bool>() ? "true" : "false";
  if (node.is_integer()) return std::to_string(*node.value_exact<int64_t>());
  if (node.is_floating_point()) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4g", *node.value_exact<double>());
    return text;
  }
  if (node.is_string()) return *node.value_exact<std::string>();
  return {};
}

}  // namespace

void Settings::Load() {
  values_.clear();
  error_.clear();
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) return;
  try {
    const toml::table table = toml::parse_file(path_.string());
    for (const KameoSettingSpec& spec : kKameoSettings) {
      if (const toml::node* node = table.get(spec.name)) values_[spec.name] = NodeToString(*node);
    }
  } catch (const toml::parse_error& failure) {
    error_ = std::string("kameo_settings.toml could not be read: ") + std::string(failure.description());
  }
}

bool Settings::Save() {
  const std::string text = KameoFormatSettingsFile([this](const KameoSettingSpec& spec, std::string& value) {
    value = Get(spec.name);
    return true;
  });
  const std::filesystem::path temp = path_.string() + ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
      error_ = "Could not write " + temp.string();
      return false;
    }
    file << text;
  }
  std::error_code ec;
  std::filesystem::rename(temp, path_, ec);
  if (ec) {
    error_ = "Could not replace " + path_.string() + ": " + ec.message();
    return false;
  }
  error_.clear();
  return true;
}

std::string Settings::Get(const std::string& name) const {
  if (const auto found = values_.find(name); found != values_.end()) return found->second;
  const KameoSettingSpec* spec = KameoFindSetting(name);
  return spec ? spec->default_value : "";
}

int Settings::GetInt(const std::string& name) const {
  const std::string text = Get(name);
  int value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

double Settings::GetDouble(const std::string& name) const {
  const std::string text = Get(name);
  return text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr);
}

void Settings::Set(const std::string& name, const std::string& value) {
  if (Get(name) == value && values_.count(name)) return;
  values_[name] = value;
  Save();
}

void Settings::SetDouble(const std::string& name, double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", value);
  Set(name, text);
}

void Settings::Reset(const std::string& name) {
  const KameoSettingSpec* spec = KameoFindSetting(name);
  if (spec) Set(name, spec->default_value);
}

bool ReadMainConfigFullscreen(const std::filesystem::path& config_path) {
  try {
    const toml::table table = toml::parse_file(config_path.string());
    if (auto value = table["fullscreen"].value<bool>()) return *value;
  } catch (const toml::parse_error&) {
  }
  return true;
}

std::string ReadMainConfigUserDataRoot(const std::filesystem::path& config_path) {
  try {
    const toml::table table = toml::parse_file(config_path.string());
    if (auto value = table["user_data_root"].value<std::string>()) return *value;
  } catch (const toml::parse_error&) {
  }
  return {};
}

}  // namespace kameo::launcher
