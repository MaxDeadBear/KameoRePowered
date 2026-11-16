#include "app/app.h"

#include "kameorepowered_init.h"

#include <cstdio>
#include <format>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/kernel/title_paths.h>
#include <rex/logging/logger.h>
#include <rex/path_settings.h>
#include <rex/platform/file.h>
#include <rex/ui/ui_system.h>
#include <rex/xam/achievement_manager.h>
#if KAMEO_GUIDE
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rexglue_guide/guide.h>

#include "gpu/ui/ui_image.h"
#endif
#if KAMEO_SENTRY
#include <sentrywork/sentrywork.h>
#endif

#include "app/process.h"
#include "app/window_mode.h"
#include "core/cheat_guard.h"
#include "core/crash_reporter.h"
#include "core/cvar.h"
#include "core/fp_guard.h"
#include "core/settings.h"
#include "engine/audio.h"
#include "engine/custom_levels.h"
#include "engine/dlc/dlc_models.h"
#include "engine/dlc/mod_costumes.h"
#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "gpu/log.h"
#include "gpu/replay/screen_layout.h"
#include "gpu/shaders/shader_mods.h"
#include "gpu/textures/texture_pack.h"
#include "gpu/ui/overlay_renderer.h"
#include "input/input.h"
#include "mods/mods.h"
#include "ui/dialog.h"
#include "ui/hud/achievement_icons.h"
#include "ui/hud/achievement_toast.h"
#include "ui/hud/fps_overlay.h"
#include "ui/hud/loading_overlay.h"
#include "ui/hud/models_overlay.h"
#include "ui/hud/shader_editor_overlay.h"
#include "ui/hud/shader_list_overlay.h"
#include "ui/name_entry/name_entry_keys.h"
#include "ui/pause_menu/pause_card.h"
#include "ui/pause_menu/pause_menu.h"

KCVAR_DEFINE_BOOL(kameo_rexliveless, true, "Kameo", "Show the Xbox 360 Guide from RexLiveless (F10 or the Guide button): sign-in, friends, achievements and online play through rexliveless.xyz. Takes effect on the next start.");

KCVAR_DEFINE_BOOL(kameo_dev_shader_editor, false, "Kameo", "For testing: open the shader editor (F1) as the game starts.");

KCVAR_DEFINE_BOOL(kameo_dev_models_window, false, "Kameo", "For testing: open the Models window (F11) as the game starts.");

namespace {

// The game's data folder, set once the title paths are known (OnInit); the Models window copies new costumes from it.
std::filesystem::path g_game_root;

#if KAMEO_GUIDE
// The images behind the Guide's textures, by the id ImGui draws them with.
std::mutex g_guide_images_mutex;
std::unordered_map<ImTextureID, std::unique_ptr<kameo::gpu::UiImage>> g_guide_images;
#endif

bool ImGuiWindowTakesMouse() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!context) return false;
  for (ImGuiWindow* window : context->Windows) {
    if (!window->Active || window->Hidden || window->IsFallbackWindow) continue;
    if (window->Flags & (ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) continue;
    return true;
  }
  return false;
}

// Our keys over the game, read where ImGui's key state lives: on the UI thread, inside the overlay frame. The console (`) and the debug overlay (F3) are the SDK's own.
class KameoHotkeys final : public KameoDialog {
 protected:
  void OnDraw(ImGuiIO&) override {
    KameoSetImGuiPointerWanted(ImGuiWindowTakesMouse());
#ifdef _WIN32
    if (KCVAR_GET(kameo_dev_shader_editor) && !opened_editor_at_start_) {
      opened_editor_at_start_ = true;
      shader_editor_ = std::make_unique<KameoShaderEditorOverlay>();
    }
    // F1 shows and hides the editor's window; the editor itself stays for the session, so the graph being worked on is kept.
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
      if (shader_editor_) shader_editor_->SetVisible(!shader_editor_->Visible());
      else shader_editor_ = std::make_unique<KameoShaderEditorOverlay>();
    }
    // F11 shows and hides the Models window; it is made on first use and then kept, with the files imported on it.
    if (!g_game_root.empty()) {
      if (KCVAR_GET(kameo_dev_models_window) && !opened_models_at_start_) {
        opened_models_at_start_ = true;
        models_ = std::make_unique<KameoModelsOverlay>(g_game_root);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        if (models_) models_->SetVisible(!models_->Visible());
        else models_ = std::make_unique<KameoModelsOverlay>(g_game_root);
      }
    }
#endif
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
      if (shader_list_) shader_list_.reset();
      else shader_list_ = std::make_unique<KameoShaderListOverlay>();
    }
    // Options and Achievements are pages of the game's own menu, not ImGui: F4 and F6 open them there.
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) KameoRequestMenuPage(KameoMenuHotkey::Options);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) KameoRequestMenuPage(KameoMenuHotkey::Achievements);
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false) && KCVAR_GET(kameo_dev_log_hud)) kameo::gpu::RequestHudFrameDump();
    // F8 flips texture replacements in play, as the Mods page's switch does, to compare a pack with the original textures.
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
      KCVAR_SET(kameo_gfx_texture_replacements, !KCVAR_GET(kameo_gfx_texture_replacements));
      kameo::gpu::RefreshAllTextureReplacements();
      KameoSaveSettings();
      REXLOG_INFO("[kameo] texture replacements {}", KCVAR_GET(kameo_gfx_texture_replacements) ? "on" : "off");
    }
    CrashIfTestKeyHeld();
  }

 private:
  std::unique_ptr<KameoShaderListOverlay> shader_list_;
#ifdef _WIN32
  std::unique_ptr<KameoShaderEditorOverlay> shader_editor_;
  bool opened_editor_at_start_ = false;
  std::unique_ptr<KameoModelsOverlay> models_;
  bool opened_models_at_start_ = false;
#endif
};

std::filesystem::path GameRoot(rex::kernel::TitleRuntime& title) { return title.GetTitlePaths().game_root.Get(); }

}  // namespace

KameoApp::KameoApp() : TitleApp("kameorepowered", kPPCImageConfig) {
#ifdef _WIN32
  // The window is created later, in Init, so DPI awareness declared here still lands before it exists.
  kameo::app::DeclarePerMonitorDpiAwareness();
#endif
}

KameoApp::~KameoApp() = default;

bool KameoApp::UseNativeRenderer() const {
  if (native_renderer_ >= 0) return native_renderer_ != 0;
  bool native = kameo::gpu::NativeRendererEnabled();
  // Releases don't ship the SDK's Xenos drivers, so a setting that asks for them falls back to the native renderer there.
  if (!native) {
    std::error_code ec;
    bool driver = false;
    for (const auto& entry : std::filesystem::directory_iterator(rex::platform::GetExecutableFolder(), ec)) {
      const std::string name = entry.path().filename().string();
      driver = driver || name.find("rexgpu-xenos-d3d12") != std::string::npos || name.find("rexgpu-xenos-vulkan") != std::string::npos;
    }
    if (!driver) {
      REXLOG_WARN("[kameo-gfx] kameo.native_renderer is off but the SDK's Xenos drivers aren't installed; using the native renderer");
      native = true;
    }
  }
  native_renderer_ = native ? 1 : 0;
  return native;
}

// The native renderer takes the place of an SDK graphics driver, so the app asks for none: the graphics system then answers the game's GPU registers itself and runs no command processor, and the renderer presents on its own.
rex::AppSysRequirements KameoApp::GetRequirements() const {
  rex::AppSysRequirements requirements = TitleApp::GetRequirements();
  if (UseNativeRenderer()) requirements.graphics_family = rex::GraphicsFamily::kNone;
  return requirements;
}

// Before the SDK reads kameorepowered.toml, the environment and the command line: our settings join the registry, and the defaults that differ from the SDK's go in under whatever the player sets.
rex::Result<void> KameoApp::OnConfigure(rex::SettingsRegistry& settings) {
  REX_TRY_VOID(kameo::cvar::RegisterAll(settings));
  REX_TRY_VOID(KameoModsConfigure(settings));
  REX_TRY_VOID(REX_LOGGER().AddCategories(rex::log::kKameoLog));
#if KAMEO_SENTRY
  REX_TRY_VOID(sentrywork::RegisterSettings(settings));
#endif
  // As early as a crash can be logged: a report from another machine otherwise ends at the last ordinary line.
  InstallCrashReporter();
#ifdef _WIN32
  // plume reports a refused resource creation on stderr only, which a windowed app has nowhere to show; it lands beside the log instead.
  {
    const std::filesystem::path path = rex::platform::GetExecutableFolder() / "logs" / "stderr.txt";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    FILE* redirected = nullptr;
    if (freopen_s(&redirected, path.string().c_str(), "a", stderr) == 0 && redirected != nullptr) setvbuf(stderr, nullptr, _IONBF, 0);
  }
#endif
  // The game data is the assets folder next to the executable unless title.game_root names another, on the command line or in kameorepowered.toml; a wrong folder fails with its path rather than being swapped for some other.
  const std::filesystem::path assets = rex::platform::GetExecutableFolder() / "assets";
  REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "title.game_root", assets.string()));
  // Saves, unlocks and installed DLC stay where they have always been (Documents\kameorepowered on Windows), with the shader cache inside; the SDK's own default is a folder beside the executable, which a new build would start empty.
  std::filesystem::path user_root = rex::platform::GetUserFolder() / "kameorepowered";
  // A Documents folder that points nowhere (a disconnected drive, an unlinked OneDrive) cannot hold them: the first save then failed inside the runtime and took the game down. The folder beside the game stands in.
  std::error_code ec;
  std::filesystem::create_directories(user_root, ec);
  if (ec) {
    REXLOG_WARN("the save folder {} cannot be created ({}); saving beside the game instead", user_root.string(), ec.message());
    user_root = rex::platform::GetExecutableFolder() / "user";
  }
  REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "app.user_root", user_root.string()));
  REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "app.cache_root", "{app.user_root}/cache"));
  // The SDK caps what a title may commit at the console's share of its 512 MB. The renderer's own guest blocks (the vertex sink, the larger shadow maps) come out of the same count and a level load ran into the cap, so there is none.
  (void)settings.SetText(rex::SettingLayer::kTitle, "runtime.title_memory_mb", "0");
  // F4 and F6 open pages of the game's own menu and F7 our shader list, so the SDK's settings and achievements overlays give those keys up.
  (void)settings.SetText(rex::SettingLayer::kTitle, "ui.bind.settings", "");
  (void)settings.SetText(rex::SettingLayer::kTitle, "ui.bind.achievements", "");
  // The menu's own settings file, whose values go over kameorepowered.toml's and under the command line's.
  KameoLoadSettings();
  return rex::Ok();
}

// A disc extracted one folder deeper than asked (assets\<disc name>\default.xex) is taken as it is when it is the only candidate, and the title update's files are the TU folder inside the game's.
void KameoApp::ResolveGameFolders() {
  namespace fs = std::filesystem;
  rex::kernel::TitlePaths& paths = GetTitleRuntime().GetTitlePaths();
  fs::path game_root = paths.game_root.Get();
  std::error_code ec;
  if (!fs::exists(game_root / "default.xex", ec) && fs::is_directory(game_root, ec)) {
    fs::path nested;
    int candidates = 0;
    for (const auto& entry : fs::directory_iterator(game_root, ec)) {
      if (entry.is_directory(ec) && fs::exists(entry.path() / "default.xex", ec)) {
        nested = entry.path();
        ++candidates;
      }
    }
    if (candidates == 1) {
      REXLOG_WARN("game data found one folder down, in {}; the expected layout is default.xex directly inside {}", nested.string(), game_root.string());
      game_root = nested;
      (void)paths.game_root.Lock(game_root);
    }
  }
  // The first thing a report needs when the game cannot find its files.
  REXLOG_INFO("game data folder: {}", game_root.string());
#if KAMEO_TU
  // The title update's data partition, which its code reads its new per-language text from (update:\English\...).
  if (paths.update_root.Get().empty()) (void)paths.update_root.Lock(game_root / "TU");
  // The title-update build loads the game from assets\TU, whose default.xexp the loader applies as the update; the vanilla build's assets\default.xex has no patch beside it, so it never picks the update up.
  (void)paths.entry.Lock("TU\\default.xex");
#endif
}

// Every source has been read and the window is open; the title has not been loaded yet.
rex::Result<void> KameoApp::OnInit() {
  KameoNoteRunningLanguage();
  ResolveGameFolders();
  const std::filesystem::path user_root = REX_PATHS().user_root.Get();
  const std::filesystem::path game_root = GameRoot(GetTitleRuntime());
  g_game_root = game_root;
  // A save folder the player named that cannot be made fails here with its path, not later in the middle of a save.
  {
    std::error_code ec;
    std::filesystem::create_directories(user_root, ec);
    if (ec) return rex::Err(rex::ErrorCategory::kIO, std::format("The save folder {} cannot be created ({}). Set app.user_root in kameorepowered.toml to a folder that exists.", user_root.string(), ec.message()));
  }
  KameoInitCheatGuard(user_root);
#if KAMEO_SENTRY
  // After the settings, which hold crash_reports, and before the title, whose exception handler has to follow Sentry's. It stays off in builds without a DSN.
  sentrywork::Init({.app_name = "kameorepowered", .tags = {{"game", "Kameo: Elements of Power"}}});
#endif
  InstallCrashReporter();
  // The RexLiveless Guide sees the controller's Guide button in the game's own pad reads, which carry it only with hid.guide_button on; a player's own choice still decides.
  if (KCVAR_GET(kameo_rexliveless) && kameo::cvar::IsDefault("guide_button")) (void)kameo::cvar::SetText(rex::SettingLayer::kTitle, "guide_button", "true");
  SyncKameoDlcListForCustomModels(user_root);
  kameo::engine::BuildModCostumePackage(user_root);
  kameo::engine::LoadCustomLevels(game_root);
  // Shader mods read the textures they name from the game's own archives.
  kameo::gpu::SetShaderModDataRoot(game_root);
  // The pause card's art is kept as the game loads it, which starts before any menu exists.
  KameoKeepPauseCardArt();
  // Before the title opens its data, so the merged folder is there when the game first asks for it.
  kameo::app::BuildMergedLanguageDir(game_root);
#ifdef _WIN32
  veh_handle_ = InstallGuestFpExceptionHandlerWin();
#endif
  if (UseNativeRenderer()) REX_TRY_VOID(StartNativeRenderer());
  KameoInitWindowMode();
  KameoInitInput();
  // After the Guide, whose menu the mods menu joins.
  REX_TRY_VOID(KameoModsInit(GetUI()));
  return rex::Ok();
}

rex::Result<void> KameoApp::StartNativeRenderer() {
  kameo_graphics_ = std::make_unique<kameo::gpu::GraphicsSystem>([]() -> void* { return kameo::gpu::NativeWindowHandle(); }, kameo::gpu::NativeRendererBackend());
  if (!kameo_graphics_->Initialize()) {
    kameo_graphics_.reset();
    return rex::Err(rex::ErrorCategory::kPlatform, "The native renderer could not start: no Direct3D 12 or Vulkan device was created. Update the graphics driver, or set kameo.gfx_backend in kameorepowered.toml.");
  }
  REXLOG_INFO("[kameo-gfx] native renderer enabled");
  kameo_overlay_renderer_ = std::make_unique<kameo::gpu::OverlayRenderer>(kameo_graphics_.get());
  ImFontAtlas& fonts = GetUI().GetFonts();
  KameoLoadingOverlay::ConfigureFonts(&fonts);
#if KAMEO_GUIDE
  if (KCVAR_GET(kameo_rexliveless)) rexglue_guide::ConfigureFonts(&fonts);
#endif
  CreateDialogs();
  // Called on the game thread at each present: the UI system builds the overlay frame on the UI thread while this thread waits, and the overlay renderer hands its geometry to the render thread.
  kameo_graphics_->SetUiFrameCallback([this](uint32_t width, uint32_t height) {
    kameo_overlay_renderer_->SetTargetSize(width, height);
    GetUI().DrawOverlays(*kameo_overlay_renderer_);
    GetGraphics().ReportFrame();
  });
  return rex::Ok();
}

void KameoApp::CreateDialogs() {
  kameo_name_entry_keys_ = std::make_unique<KameoNameEntryKeys>();
  kameo_fps_overlay_ = std::make_unique<KameoFpsOverlay>();
  kameo_loading_overlay_ = std::make_unique<KameoLoadingOverlay>();
  kameo_achievement_icons_ = std::make_unique<KameoAchievementIcons>();
  kameo_pause_card_ = std::make_unique<KameoPauseCard>();
  kameo_hotkeys_ = std::make_unique<KameoHotkeys>();
#if KAMEO_GUIDE
  // Last, so the Guide draws over the game's own overlays.
  if (KCVAR_GET(kameo_rexliveless)) {
    rexglue_guide::Install(GetUI(), {
        .title_name = "Kameo: Elements of Power",
        .on_quit = [] { KameoQuitApplication(); },
        // The Guide's pictures are native-renderer images, kept here until it frees them.
        .create_texture = [](uint32_t width, uint32_t height, const uint8_t* rgba) -> ImTextureID {
          std::unique_ptr<kameo::gpu::UiImage> image = kameo::gpu::UiImage::Create(width, height, rgba);
          if (!image) return 0;
          const ImTextureID id = image->texture_id();
          std::lock_guard lock(g_guide_images_mutex);
          g_guide_images[id] = std::move(image);
          return id;
        },
        .destroy_texture = [](ImTextureID id) {
          std::lock_guard lock(g_guide_images_mutex);
          g_guide_images.erase(id);
        },
        .leaderboard_block = [] { return KameoLeaderboardBlock(); },
        .build = KAMEO_BUILD_NAME,
    });
    kameo_guide_installed_ = true;
  }
#endif
}

// The title is loaded and its guest threads have not started.
rex::Result<void> KameoApp::OnTitleLaunch(rex::kernel::TitleRuntime& title) {
  uint32_t lang = kameo::app::ResolveUserLanguage();
  g_kameo_startup_language.store(static_cast<int32_t>(lang), std::memory_order_relaxed);
  // Primed so XGetLanguage is overridden before the opening FMV picks its Bink track.
  if (lang > 1) {
    g_kameo_audio_language.store(static_cast<int32_t>(lang), std::memory_order_relaxed);
    kameo::app::RegisterLanguageRedirects(title.GetFileSystem(), lang);
  }
  // Unlocks show in the game's own text box. The Guide shows its own unlock pop-up, so the text box stands down while it is installed.
  kameo_achievement_listener_ = title.GetAchievementManager().RegisterNotificationCallback([this](const rex::xam::AchievementEvent& event) {
    if (!kameo_guide_installed_) KameoPushAchievementToast(event);
  });
#if KAMEO_SENTRY
  sentrywork::OnRuntimeReady(&title);
#endif
  // The enabled mods load now: guest memory exists and no guest code has run.
  KameoModsStart();
  // Sentry and the runtime take the last-chance filter here, so the local report goes back on top: crash_*.txt and .dmp are still written, the player is asked before the crash goes on to Sentry, and the log Sentry attaches carries the guest function names.
  InstallCrashReporter();
  CrashForTestIfAsked();
#ifndef _WIN32
  // Installed after the runtime is set up so it overrides any SIGFPE handler of the SDK's.
  veh_handle_ = InstallGuestFpExceptionHandlerPosix();
#endif
  return rex::Ok();
}

// A registration with the title's achievement manager is undone through that manager, and Shutdown unloads the title, manager and all, before OnShutdown runs: undone there, it read freed memory as the game closed.
void KameoApp::ReleaseTitleListeners() {
  // First, while the game their hooks are in still exists.
  KameoModsStop();
#if KAMEO_GUIDE
  if (kameo_guide_installed_) rexglue_guide::Shutdown();
#endif
  kameo_guide_installed_ = false;
  kameo_achievement_listener_ = rex::ScopeGuard();
  // The render thread reads guest memory, which the unload frees.
  if (kameo_graphics_) kameo_graphics_->StopRendering();
}

void KameoApp::OnShutdown() {
  KameoNoteExclusiveFullscreen(false);
  KameoShutdownInput();
  if (kameo_graphics_) kameo_graphics_->SetUiFrameCallback(nullptr);
  kameo_hotkeys_.reset();
  kameo_pause_card_.reset();
  kameo_achievement_icons_.reset();
  kameo_loading_overlay_.reset();
  kameo_fps_overlay_.reset();
  kameo_name_entry_keys_.reset();
  kameo_overlay_renderer_.reset();
  // The title's threads have stopped by now (the app unloads it before this), so nothing draws any more.
  kameo_graphics_.reset();
  RemoveGuestFpExceptionHandler(veh_handle_);
  veh_handle_ = nullptr;
#if KAMEO_SENTRY
  sentrywork::Shutdown();
#endif
}
