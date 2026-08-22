
// kameorepowered - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/platform/fpscr.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>

#include "kameorepowered_dialogs.h"
#include "kameorepowered_dlc_models.h"
#include "kameorepowered_fp_guard.h"

#ifdef KAMEO_NATIVE_RENDERER
#include "gfx/kameo_graphics_system.h"
#endif

class KameorepoweredApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<KameorepoweredApp>(new KameorepoweredApp(ctx, "kameorepowered",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#ifndef KAMEO_TU
    SyncKameoDlcListForCustomModels();
#endif
    // Build the merged language directory BEFORE the runtime mounts the game
    // data. The host VFS snapshots the directory tree (recursively) at mount
    // time, so a folder created later (e.g. in OnPostSetup) resolves but exposes
    // no children -- file opens inside it fail with NO_SUCH_FILE. The user
    // language cvar is already loaded from the config at this point.
    BuildMergedLanguageDir();
#ifdef _WIN32
    veh_handle_ = InstallGuestFpExceptionHandlerWin();
#endif

#ifdef KAMEO_NATIVE_RENDERER
    // Injecting an IGraphicsSystem here is a first-class runtime feature:
    // RuntimeConfig::gpu_plugin is only consulted when `graphics` is empty, so
    // this replaces the xenos plugin outright without any SDK change.
    //
    // The window does not exist yet -- it is created during SetupPresentation,
    // which the runtime calls on the system we are handing it -- so the handle
    // is fetched through a callback rather than captured now.
    if (kameo::gfx::NativeRendererEnabled()) {
      config.graphics = std::make_unique<kameo::gfx::KameoGraphicsSystem>([this]() -> void* {
        return window() ? window()->GetNativeWindowHandle() : nullptr;
      });
      config.gpu_plugin.clear();
      REXLOG_WARN("[kameo-gfx] native renderer enabled; the game will not be visible yet");
    }
#endif
  }

  void OnShutdown() override {
    kameo_model_dialog_.reset();
    kameo_audio_dialog_.reset();
    RemoveGuestFpExceptionHandler(veh_handle_);
    veh_handle_ = nullptr;
  }

  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostSetup() override {
    uint32_t lang = ResolveUserLanguage();
    g_kameo_startup_language.store(static_cast<int32_t>(lang),
                                   std::memory_order_relaxed);
    // Prime g_kameo_audio_language so KameoOverrideAudioLanguage overrides
    // XGetLanguage before the opening FMV plays. Without this, the game stores
    // the system language (English) at 0x827556B4 and the Bink track selection
    // uses English even when --user_language is non-English.
    if (lang > 1) {
      g_kameo_audio_language.store(static_cast<int32_t>(lang),
                                   std::memory_order_relaxed);
    }

    // The game hardcodes "D:\english" for localized data. The VFS only runs the
    // *directory* component of an open through the symlink table (OpenFile splits
    // "D:\english\file" into base "D:\english", resolved via symlinks, plus
    // "file", fetched directly as a child), so only a directory-level symlink
    // takes effect. Point "english" at the merge built in OnPreSetup; no redirect
    // is needed for English itself (lang == 1).
    if (lang > 1) {
      namespace fs = std::filesystem;
      auto* vfs = runtime()->file_system();
      vfs->RegisterSymbolicLink(
          "\\Device\\Harddisk0\\Partition1\\english",
          std::string("\\Device\\Harddisk0\\Partition1\\") + kMergedDirName);

      // The title update ships its own per-language string tables on the update
      // partition, opened as "update:\english\...". Redirect that directory too;
      // it has no stub files (and exists at mount), so a direct symlink suffices.
      std::error_code ec;
      auto update_root = runtime()->update_data_root();
      if (!update_root.empty() &&
          fs::is_directory(update_root / kLangFolders[lang], ec)) {
        vfs->RegisterSymbolicLink(
            "\\Device\\Harddisk0\\PartitionUpdate\\english",
            std::string("\\Device\\Harddisk0\\PartitionUpdate\\") + kLangFolders[lang]);
      }

      // Each DLC package carries its own per-language string tables and the game
      // hardcodes loading them from the package's "english" folder. Once a DLC is
      // installed, that table *shadows* the base game's, so the DLC's english
      // folder dictates the whole UI language -- redirecting it is what actually
      // localizes the game when DLC is present. The DLC language folders hold only
      // the string table (no stubs), so a direct symlink to the localized folder
      // is safe.
      //
      // DLC content devices mount dynamically at \Device\Content\<n>\; symlinks
      // are global and outlive device lifetime, so registering a fixed range of
      // slots up front is enough (slots without an english folder stay inert).
      // The dlc<n>: alias keeps a trailing separator, so the resolved path has a
      // doubled separator before "english" -- register that form, plus the clean
      // single-separator form for safety.
      const std::string lang_folder = kLangFolders[lang];
      for (int n = 1; n <= kMaxContentSlots; ++n) {
        std::string slot = "\\Device\\Content\\" + std::to_string(n) + "\\";
        vfs->RegisterSymbolicLink(slot + "english", slot + lang_folder);
        vfs->RegisterSymbolicLink(slot + "\\english", slot + "\\" + lang_folder);
      }
    }
#ifndef _WIN32
    // Install after SDK setup so we override any SDK-installed SIGFPE handler.
    veh_handle_ = InstallGuestFpExceptionHandlerPosix();
#endif
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    kameo_model_dialog_ = std::make_unique<KameoModelDialog>(drawer);
    kameo_audio_dialog_ = std::make_unique<KameoAudioDialog>(drawer);
  }
  // Locate the game data next to (or above) the executable so the build can be
  // run straight out of its build directory without --game_data_root. Only
  // fills in when the computed default does not actually contain the game, so
  // an explicit --game_data_root / cvar always wins.
  //
  // update_data_root is set only when the TU patch is staged: the loader
  // applies assets/default.xexp on every launch, so its presence is what
  // distinguishes a TU install from a vanilla one.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(paths.game_data_root / kEntryXexName, ec)) {
      return;  // caller gave us a usable root
    }
    for (auto dir = rex::filesystem::GetExecutableFolder(); !dir.empty();
         dir = dir.parent_path()) {
      auto assets = dir / "assets";
      if (!fs::exists(assets / kEntryXexName, ec)) {
        if (!dir.has_relative_path()) {
          break;  // hit the filesystem root
        }
        continue;
      }
      paths.game_data_root = assets;
      // Sibling '<xex>p' -- the patch the loader re-applies to the base image.
      if (fs::exists(assets / (std::string(kEntryXexName) + "p"), ec) &&
          fs::is_directory(assets / kUpdateDirName, ec)) {
        paths.update_data_root = assets / kUpdateDirName;
      }
      break;
    }
  }

 private:
  static constexpr const char* kEntryXexName = "default.xex";
  // Update partition holding the TU's per-language .str string tables,
  // extracted by scripts/extract_tu.py --update-dir.
  static constexpr const char* kUpdateDirName = "TU";

  // Per-language data folders shipped in the game data; index == XLanguage id.
  // The game hardcodes "D:\english" regardless of the selected language.
  static constexpr const char* kLangFolders[] = {
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
  // Hard-linked merge of English + the localized files, built under the game
  // data root and pointed to by the "english" symlink.
  static constexpr const char* kMergedDirName = ".rex_lang_merged";

  // Upper bound on DLC content-device slots (\Device\Content\<n>\) to register
  // language redirects for. The game uses far fewer; extra slots are inert.
  static constexpr int kMaxContentSlots = 16;

  static uint32_t ResolveUserLanguage() {
    uint32_t lang = REXCVAR_GET(user_language);
    if (lang == 0 || lang >= std::size(kLangFolders) || !kLangFolders[lang]) {
      lang = 1;  // Default to English if the setting is missing or invalid.
    }
    return lang;
  }

  // Materialize kMergedDirName = English (base) overlaid with the localized
  // files. Two kinds of files must stay English or the HUD icons disappear:
  //   - 16-byte stub .plf files (filtered by the > 64 byte check), and
  //   - the front-end .lvl, which localizations ship in a "slightly different"
  //     form that drops the bottom-screen ability icons. The translated text
  //     lives in the .str string table (still overlaid), not the .lvl, so
  //     keeping the English .lvl loses nothing visible but the icons.
  // Hard links keep this free of extra disk use; a copy is used as a fallback
  // when linking is unavailable (e.g. cross-device). Idempotent per launch.
  void BuildMergedLanguageDir() {
    uint32_t lang = ResolveUserLanguage();
    if (lang <= 1) {
      return;  // English needs no redirect.
    }
    namespace fs = std::filesystem;
    auto game_root = game_data_root();
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
    // Overlay: localized files with real content (stubs are <= 64 bytes),
    // except .lvl files which must stay English to keep the HUD icons.
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

  void* veh_handle_ = nullptr;
  std::unique_ptr<KameoModelDialog> kameo_model_dialog_;
  std::unique_ptr<KameoAudioDialog> kameo_audio_dialog_;
};
