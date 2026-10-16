// The language redirect: the game hardcodes "D:\english", so that folder is pointed at English overlaid with the chosen language's files.

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/flags.h>

#include "app/app.h"

namespace kameo::app {
namespace {

// Per-language data folders shipped in the game data; index == XLanguage id.
constexpr const char* kLangFolders[] = {
  nullptr,      // 0 invalid
  "English",    // 1
  "Japanese",   // 2
  "German",     // 3
  "French",     // 4
  "Spanish",    // 5
  "Italian",    // 6
  "Korean",     // 7
  "TChinese",   // 8
  "Portuguese", // 9
  "SChinese",   // 10
  "Polish",     // 11
  "Russian",    // 12
};
// Hard-linked merge of English and the localized files, built under the game data root.
constexpr const char* kMergedDirName = ".rex_lang_merged";
// DLC content-device slots (\Device\Content\<n>\) given a redirect; the game uses far fewer and extra slots are inert.
constexpr int kMaxContentSlots = 16;

}  // namespace

uint32_t ResolveUserLanguage() {
  uint32_t lang = REXCVAR_GET(user_language);
  if (lang == 0 || lang >= std::size(kLangFolders) || !kLangFolders[lang]) {
    lang = 1;  // Default to English if the setting is missing or invalid.
  }
  return lang;
}

// Localized 16-byte stub .plf files and the localized front-end .lvl, which drops the HUD ability icons, stay English; hard links fall back to copies across devices.
void BuildMergedLanguageDir(const std::filesystem::path& game_root) {
  uint32_t lang = ResolveUserLanguage();
  if (lang <= 1) {
    return;  // English needs no redirect.
  }
  namespace fs = std::filesystem;
  if (game_root.empty()) {
    return;
  }

  auto link_or_copy = [](const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove(dst, ec);
    fs::create_hard_link(src, dst, ec);
    if (ec) {  // cross-device or unsupported FS: fall back to a real copy.
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
  };

  std::error_code ec;
  auto merged = game_root / kMergedDirName;
  fs::remove_all(merged, ec);
  fs::create_directories(merged, ec);

  // Base layer: English (the folder the game actually asks for).
  auto eng_dir = game_root / kLangFolders[1];
  if (fs::is_directory(eng_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(eng_dir)) {
      if (entry.is_regular_file()) {
        link_or_copy(entry.path(), merged / entry.path().filename());
      }
    }
  }
  // Overlay: localized files with real content (stubs are <= 64 bytes), except .lvl files.
  auto lang_dir = game_root / kLangFolders[lang];
  if (fs::is_directory(lang_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(lang_dir)) {
      if (!entry.is_regular_file() || entry.file_size() <= 64) {
        continue;
      }
      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (ext == ".lvl") {
        continue;  // keep the English front-end level (icons).
      }
      link_or_copy(entry.path(), merged / entry.path().filename());
    }
  }
}

// The VFS only resolves the directory part of an open through symlinks, so only a folder-level link works; each DLC package's own "english" string table shadows the base game's, so those are redirected too.
void RegisterLanguageRedirects(rex::filesystem::VirtualFileSystem& vfs, uint32_t lang) {
  vfs.RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\english",
      std::string("\\Device\\Harddisk0\\Partition1\\") + kMergedDirName);

  // The dlc<n>: alias keeps a trailing separator, so the doubled-separator form is registered as well as the clean one.
  const std::string lang_folder = kLangFolders[lang];
  for (int n = 1; n <= kMaxContentSlots; ++n) {
    std::string slot = "\\Device\\Content\\" + std::to_string(n) + "\\";
    vfs.RegisterSymbolicLink(slot + "english", slot + lang_folder);
    vfs.RegisterSymbolicLink(slot + "\\english", slot + "\\" + lang_folder);
  }
}

}  // namespace kameo::app
