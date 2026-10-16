// kameo_settings.toml: the pause-menu options, saved whenever one changes and loaded after the main config at startup.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>

#include "app/process.h"
#include "app/window_mode.h"
#include "core/settings.h"
#include "core/settings_schema.h"
#include "engine/audio.h"
#include "input/input.h"

REXCVAR_DEFINE_INT32(kameo_volume_master, 50, "Kameo", "Master volume, 0-100.");
REXCVAR_DEFINE_INT32(kameo_volume_music, 50, "Kameo", "Music volume, 0-100.");
REXCVAR_DEFINE_INT32(kameo_volume_sfx, 50, "Kameo", "Sound effects volume, 0-100.");
REXCVAR_DEFINE_INT32(kameo_volume_ambience, 50, "Kameo", "Ambience volume, 0-100.");
REXCVAR_DEFINE_INT32(kameo_volume_speech, 50, "Kameo", "Speech volume, 0-100.");
REXCVAR_DEFINE_INT32(kameo_volume_cutscenes, 50, "Kameo", "Cutscene (FMV) volume, 0-100.");
REXCVAR_DEFINE_INT32(kameo_language, 0, "Kameo", "Language applied at the next launch: 0 keeps user_language, otherwise 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Traditional Chinese.");
REXCVAR_DECLARE(uint32_t, user_language);

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

std::filesystem::path SettingsFolder() { return rex::filesystem::GetExecutableFolder(); }

}  // namespace

void KameoLoadSettings() {
  const std::filesystem::path path = SettingsFolder() / kSettingsFile;
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    rex::cvar::LoadConfig(path);
    REXLOG_INFO("[kameo] loaded settings from {}", path.string());
  }
  KameoApplyDefaultControls();
  // The language is only ever changed here, before the language folders are merged and mounted, because switching it mid-session broke the game.
  const int32_t language = REXCVAR_GET(kameo_language);
  if (language >= 1 && language <= 8 && rex::cvar::GetFlagSource("user_language") != rex::cvar::Source::kCommandLine) REXCVAR_SET(user_language, uint32_t(language));
  g_running_language = REXCVAR_GET(user_language);
  for (const VolumeBinding& binding : kVolumes) binding.volume->store(std::clamp(rex::cvar::Query<int32_t>(binding.name), 0, 100));
  g_kameo_volume_dirty.store(1);
  g_loaded = true;
  kameo::app::RecoverFromCrashedFullscreen();
  // The runtime creates its window from the fullscreen flag, so a saved fullscreen mode has to be in it before then; otherwise the game opens as an empty 1280x720 window over a black screen and only goes fullscreen afterwards.
  const int32_t window_mode = rex::cvar::Query<int32_t>("kameo_window_mode");
  if (window_mode >= 0 && window_mode <= 2 && rex::cvar::GetFlagSource("fullscreen") != rex::cvar::Source::kCommandLine) rex::cvar::SetFlagByName("fullscreen", window_mode == 0 ? "false" : "true");
}

void KameoSaveSettings() {
  if (!g_loaded) return;
  std::lock_guard<std::mutex> lock(g_save_mutex);
  for (const VolumeBinding& binding : kVolumes) rex::cvar::SetFlagByName(binding.name, std::to_string(binding.volume->load()));
  const std::string text = KameoFormatSettingsFile([](const KameoSettingSpec& spec, std::string& value) {
    if (!rex::cvar::GetFlagInfo(spec.name)) return false;
    const std::string name = spec.name;
    value = name == "kameo_window_mode" ? std::to_string(KameoWindowMode()) : name.starts_with("keybind_") ? KameoKeybindForSave(name) : rex::cvar::GetFlagByName(name);
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
    rex::cvar::ResetToDefault(name);
    controls = controls || name.starts_with("keybind_") || name == "mnk_mouse";
  }
  // The PC bindings and mouse camera are layered over the runtime's own defaults, which the reset just put back.
  if (controls) KameoApplyDefaultControls();
  for (const VolumeBinding& binding : kVolumes) {
    if (std::find(names.begin(), names.end(), binding.name) == names.end()) continue;
    binding.volume->store(std::clamp(rex::cvar::Query<int32_t>(binding.name), 0, 100));
    g_kameo_volume_dirty.store(1);
  }
  if (KameoWindowMode() != window_mode_before) KameoApplyWindowMode();
  KameoSaveSettings();
}

uint32_t KameoRunningLanguage() { return g_running_language; }
