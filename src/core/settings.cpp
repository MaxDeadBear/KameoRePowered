// kameo_settings.toml: the pause-menu options, saved whenever one changes and loaded after the main config at startup.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "core/cvar.h"
#include <rex/logging/log_category.h>
#include <rex/platform/file.h>

#include <toml++/toml.hpp>

#include "app/process.h"
#include "app/window_mode.h"
#include "core/settings.h"
#include "core/cheat_guard.h"
#include "core/settings_schema.h"
#include "engine/audio.h"
#include "input/input.h"

KCVAR_DEFINE_INT32(kameo_volume_master, 50, "Kameo", "Master volume, 0-100.");
KCVAR_DEFINE_INT32(kameo_volume_music, 50, "Kameo", "Music volume, 0-100.");
KCVAR_DEFINE_INT32(kameo_volume_sfx, 50, "Kameo", "Sound effects volume, 0-100.");
KCVAR_DEFINE_INT32(kameo_volume_ambience, 50, "Kameo", "Ambience volume, 0-100.");
KCVAR_DEFINE_INT32(kameo_volume_speech, 50, "Kameo", "Speech volume, 0-100.");
KCVAR_DEFINE_INT32(kameo_volume_cutscenes, 50, "Kameo", "Cutscene (FMV) volume, 0-100.");
KCVAR_DEFINE_INT32(kameo_language, 0, "Kameo", "Language applied at the next launch: 0 keeps user_language, otherwise 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Traditional Chinese.");

namespace {

constexpr const char* kSettingsFile = "kameo_settings.toml";

struct VolumeBinding {
  const char* name;
  std::atomic<int32_t>* volume;
};

const VolumeBinding kVolumes[] = {
    {"kameo_volume_master", &g_kameo_master_volume},
    {"kameo_volume_music", &g_kameo_music_volume},
    {"kameo_volume_sfx", &g_kameo_sfx_volume},
    {"kameo_volume_ambience", &g_kameo_ambience_volume},
    {"kameo_volume_speech", &g_kameo_speech_volume},
    {"kameo_volume_cutscenes", &g_kameo_fmv_volume},
};

std::mutex g_save_mutex;
bool g_loaded = false;
uint32_t g_running_language = 1;

std::filesystem::path SettingsFolder() { return rex::platform::GetExecutableFolder(); }

// The file's values go in at this layer: over kameorepowered.toml, which the SDK reads afterwards at the file layer, and
// under the command line, which it reads after that. The values derived from them below go in at the same layer.
constexpr rex::SettingLayer kMenuLayer = rex::SettingLayer::kEnvironment;

// The file is read here rather than by the registry, so that names from before SDK 0.11 apply without a notice each.
void LoadSettingsFile(const std::filesystem::path& path) {
  toml::table table;
  try {
    std::ifstream file(path, std::ios::binary);
    table = toml::parse(file, path.filename().string());
  } catch (const toml::parse_error& error) {
    REXLOG_WARN("[kameo] {} was not read: {}", path.string(), std::string(error.description()));
    return;
  }
  const auto apply = [](const std::string& name, const toml::node& node) {
    std::string text;
    if (const auto value = node.value<bool>(); node.is_boolean() && value) text = *value ? "true" : "false";
    else if (const auto integer = node.value<int64_t>(); node.is_integer() && integer) text = std::to_string(*integer);
    else if (const auto number = node.value<double>(); node.is_floating_point() && number) text = std::to_string(*number);
    else if (const auto string = node.value<std::string>(); node.is_string() && string) text = *string;
    else return;
    kameo::cvar::SetText(kMenuLayer, name, text);
  };
  for (const auto& [key, node] : table) {
    // A table is a scope ([kameo] gfx_vsync = true); anything else is a whole name.
    if (const toml::table* scope = node.as_table()) {
      for (const auto& [member, value] : *scope) apply(std::string(key.str()) + "." + std::string(member.str()), value);
    } else {
      apply(std::string(key.str()), node);
    }
  }
  REXLOG_INFO("[kameo] loaded settings from {}", path.string());
}

}  // namespace

void KameoLoadSettings() {
  const std::filesystem::path path = SettingsFolder() / kSettingsFile;
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) LoadSettingsFile(path);
  KameoApplyDefaultControls();
  // The language is only ever changed here, before the language folders are merged and mounted, because switching it mid-session broke the game.
  const int32_t language = KCVAR_GET(kameo_language);
  if (language >= 1 && language <= 8) kameo::cvar::SetText(kMenuLayer, "user_language", std::to_string(language));
  for (const VolumeBinding& binding : kVolumes) binding.volume->store(std::clamp(kameo::cvar::GetInt(binding.name), 0, 100));
  g_kameo_volume_dirty.store(1);
  g_loaded = true;
  kameo::app::RecoverFromCrashedFullscreen();
  // The runtime creates its window from the fullscreen setting, so a saved fullscreen mode has to be in it before then; otherwise the game opens as an empty 1280x720 window over a black screen and only goes fullscreen afterwards.
  const int32_t window_mode = kameo::cvar::GetInt("kameo_window_mode");
  if (window_mode >= 0 && window_mode <= 2) kameo::cvar::SetText(kMenuLayer, "fullscreen", window_mode == 0 ? "false" : "true");
}

void KameoNoteRunningLanguage() { g_running_language = uint32_t(kameo::cvar::GetInt("user_language", 1)); }

void KameoSaveSettings() {
  if (!g_loaded) return;
  // First, so a cheat just turned on is recorded in the file written here.
  KameoNoteCheatSettings();
  std::lock_guard<std::mutex> lock(g_save_mutex);
  for (const VolumeBinding& binding : kVolumes) kameo::cvar::SetText(binding.name, std::to_string(binding.volume->load()));
  const std::string text = KameoFormatSettingsFile([](const KameoSettingSpec& spec, std::string& value) {
    if (!kameo::cvar::Exists(spec.name)) return false;
    const std::string name = spec.name;
    value = name == "kameo_window_mode" ? std::to_string(KameoWindowMode()) : name.starts_with("keybind_") ? KameoKeybindForSave(name) : kameo::cvar::GetText(name);
    return true;
  });
  const std::filesystem::path path = SettingsFolder() / kSettingsFile;
  const std::filesystem::path temp = path.string() + ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
      REXLOG_WARN("[kameo] could not write {}", temp.string());
      return;
    }
    file << text;
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) REXLOG_WARN("[kameo] could not replace {}: {}", path.string(), ec.message());
}

void KameoRestoreDefaultSettings(const std::vector<std::string>& names) {
  bool controls = false;
  const int32_t window_mode_before = KameoWindowMode();
  for (const std::string& name : names) {
    kameo::cvar::Reset(name);
    controls = controls || name.starts_with("keybind_") || name == "mnk_mouse";
  }
  // The PC bindings and mouse camera are layered over the runtime's own defaults, which the reset just put back.
  if (controls) KameoApplyDefaultControls();
  for (const VolumeBinding& binding : kVolumes) {
    if (std::find(names.begin(), names.end(), binding.name) == names.end()) continue;
    binding.volume->store(std::clamp(kameo::cvar::GetInt(binding.name), 0, 100));
    g_kameo_volume_dirty.store(1);
  }
  if (KameoWindowMode() != window_mode_before) KameoApplyWindowMode();
  KameoSaveSettings();
}

uint32_t KameoRunningLanguage() { return g_running_language; }
