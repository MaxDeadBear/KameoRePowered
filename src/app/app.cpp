#include "kameorepowered_init.h"

#include "app/app.h"

#include <cstdio>
#include <system_error>

#include <toml++/toml.hpp>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/api.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rexglue_guide/guide.h>
#include <sentrywork/sentrywork.h>

#include "app/process.h"
#include "app/window_mode.h"
#include "core/crash_reporter.h"
#include "core/fp_guard.h"
#include "core/settings.h"
#include "engine/audio.h"
#include "engine/dlc/dlc_models.h"
#include "input/input.h"
#include "ui/hud/achievement_toast.h"
#include "ui/pause_menu/pause_menu.h"

REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, cache_root);
REXCVAR_DEFINE_BOOL(kameo_rexliveless, true, "Kameo", "Show the Xbox 360 Guide from RexLiveless (F10 or the Guide button): sign-in, friends, achievements and online play through rexliveless.xyz. Takes effect on the next start.");

namespace {

// A path from kameorepowered.toml, read here because the SDK loads that file only after it has chosen the paths.
std::string ConfiguredPath(const std::filesystem::path& config_path, const char* key) {
  try {
    const toml::table table = toml::parse_file(config_path.string());
    if (auto value = table[key].value<std::string>()) return *value;
  } catch (const toml::parse_error&) {
  }
  return {};
}

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

// rexglue's overlays carry no visibility of their own and unregistering one only takes effect after it was drawn, so a toggle constructs and destroys.
template <typename Make>
void ToggleOverlay(ImGuiKey key, std::unique_ptr<rex::ui::ImGuiDialog>& slot, Make make) {
  if (!ImGui::IsKeyPressed(key, false)) {
    return;
  }
  if (slot) {
    slot.reset();
  } else {
    slot = make();
  }
}

}  // namespace

// The window is created later, in SetupPresentation, so DPI awareness declared here still lands before it exists.
std::unique_ptr<rex::ui::WindowedApp> KameoApp::Create(rex::ui::WindowedAppContext& ctx) {
#ifdef _WIN32
  kameo::app::DeclarePerMonitorDpiAwareness();
#endif
  return std::unique_ptr<KameoApp>(new KameoApp(ctx, "kameorepowered", PPCImageConfig));
}

void KameoApp::OnPreSetup(rex::RuntimeConfig& config) {
  // The RexLiveless Guide sees the controller's Guide button in the game's own pad reads, which carry it only with guide_button on; a config that sets it still decides.
  if (const rex::cvar::FlagEntry* flag = rex::cvar::GetFlagInfo("guide_button"); REXCVAR_GET(kameo_rexliveless) && flag && flag->source == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName("guide_button", "true");
  SyncKameoDlcListForCustomModels(user_data_root());
  // The host VFS snapshots the tree at mount time, so a folder created after this resolves but exposes no children.
  kameo::app::BuildMergedLanguageDir(game_data_root());
#ifdef _WIN32
  veh_handle_ = InstallGuestFpExceptionHandlerWin();
#endif

  // RuntimeConfig::gpu_plugin is only consulted when `graphics` is empty; the window does not exist yet, so its handle is fetched through a callback.
  if (kameo::gpu::NativeRendererEnabled()) {
    auto graphics = std::make_unique<kameo::gpu::GraphicsSystem>(
        [this]() -> void* { return kameo::gpu::NativeWindowHandle(window()); },
        kameo::gpu::NativeRendererBackend());
    kameo_graphics_ = graphics.get();
    config.graphics = std::move(graphics);
    config.gpu_plugin.clear();
    REXLOG_INFO("[kameo-gfx] native renderer enabled");
  }
}

// ReXApp only builds its overlay stack for a graphics system with a presenter or for none at all; the plume one owns its swap chain, so the same stack is built here from the public pieces.
bool KameoApp::SetupPresentation() {
  if (!ReXApp::SetupPresentation()) {
    return false;
  }
  if (kameo_graphics_ && !imgui_drawer() && window()) {
    kameo_immediate_drawer_ = std::make_unique<kameo::gpu::PlumeImmediateDrawer>(kameo_graphics_);
    kameo_imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window(), 64, [](ImFontAtlas* atlas) {
      if (REXCVAR_GET(kameo_rexliveless)) rexglue_guide::ConfigureFonts(atlas);
    });
    // A null presenter is supported: AddDialog registers the drawer's own window input listener and only touches the presenter when there is one.
    kameo_imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr,
                                                        kameo_immediate_drawer_.get());
    OnCreateDialogs(kameo_imgui_drawer_.get());
    kameo_graphics_->SetUiFrameCallback([this](uint32_t w, uint32_t h) {
      if (kameo_imgui_drawer_) {
        kameo::gpu::UiDrawContext context(w, h);
        kameo_imgui_drawer_->Draw(context);
        KameoSetImGuiPointerWanted(ImGuiWindowTakesMouse());
      }
      ToggleOverlay(ImGuiKey_GraveAccent, kameo_console_overlay_, [this] {
        return std::make_unique<rex::ui::ConsoleDialog>(kameo_imgui_drawer_.get(),
                                                        kameo_log_sink_);
      });
      ToggleOverlay(ImGuiKey_F3, kameo_debug_overlay_, [this] {
        return std::make_unique<rex::ui::DebugOverlayDialog>(kameo_imgui_drawer_.get());
      });
      // Options and Achievements are pages of the game's own menu, not ImGui: F4 and F6 open them there.
      if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) KameoRequestMenuPage(KameoMenuHotkey::Options);
      if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) KameoRequestMenuPage(KameoMenuHotkey::Achievements);
      CrashIfTestKeyHeld();
    });
  }
  return true;
}

void KameoApp::OnShutdown() {
  KameoNoteExclusiveFullscreen(false);
  if (kameo_guide_installed_) rexglue_guide::Shutdown();
  kameo_guide_installed_ = false;
  if (kameo_graphics_) {
    kameo_graphics_->SetUiFrameCallback(nullptr);
  }
  kameo_fps_overlay_.reset();
  kameo_achievement_icons_.reset();
  kameo_name_entry_keys_.reset();
  if (kameo_achievement_listener_ != 0 && runtime()) {
    achievements().UnregisterCallback(kameo_achievement_listener_);
    kameo_achievement_listener_ = 0;
  }
  kameo_debug_overlay_.reset();
  kameo_console_overlay_.reset();
  if (runtime() && runtime()->imgui_drawer() == kameo_imgui_drawer_.get()) runtime()->set_imgui_drawer(nullptr);
  kameo_imgui_drawer_.reset();
  kameo_immediate_drawer_.reset();
  RemoveGuestFpExceptionHandler(veh_handle_);
  veh_handle_ = nullptr;
  sentrywork::Shutdown();
}

void KameoApp::OnPostSetup() {
  // XamShowKeyboardUI falls back to the game's default text when the runtime has no drawer, which named every new save "Kameo" and made them share one content folder; the runtime does not exist yet during SetupPresentation.
  if (kameo_imgui_drawer_ && runtime()) runtime()->set_imgui_drawer(kameo_imgui_drawer_.get());
  KameoInitWindowMode(window());
  KameoInitInput(window(), runtime()->input_system());
  uint32_t lang = kameo::app::ResolveUserLanguage();
  g_kameo_startup_language.store(static_cast<int32_t>(lang),
                                 std::memory_order_relaxed);
  // Primed so XGetLanguage is overridden before the opening FMV picks its Bink track.
  if (lang > 1) {
    g_kameo_audio_language.store(static_cast<int32_t>(lang),
                                 std::memory_order_relaxed);
  }
  if (lang > 1) {
    kameo::app::RegisterLanguageRedirects(*runtime()->file_system(), lang);
  }
  // The SDK shows unlocks only through its own ImGui toast, built on a drawer this app leaves null; the game's text box shows them instead.
  // The Guide shows its own unlock pop-up, so the text box stands down while it is installed.
  kameo_achievement_listener_ = achievements().RegisterNotificationCallback([this](const rex::system::AchievementEvent& event) {
    if (!kameo_guide_installed_) KameoPushAchievementToast(event);
  });
  if (kameo_achievement_icons_) kameo_achievement_icons_->SetRuntime(runtime());
  // Sentry takes the last-chance filter back here and ends the process without calling the one before it, so the local report goes back on top: crash_*.txt and .dmp are still written, the player is asked before the crash goes on to Sentry, and the log Sentry attaches carries the guest function names.
  sentrywork::OnRuntimeReady(runtime());
  InstallCrashReporter();
  CrashForTestIfAsked();
#ifndef _WIN32
  // Installed after SDK setup so it overrides any SDK-installed SIGFPE handler.
  veh_handle_ = InstallGuestFpExceptionHandlerPosix();
#endif
}

void KameoApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  kameo_name_entry_keys_ = std::make_unique<KameoNameEntryKeys>(drawer);
  kameo_fps_overlay_ = std::make_unique<KameoFpsOverlay>(drawer);
  kameo_achievement_icons_ = std::make_unique<KameoAchievementIcons>(drawer, kameo_immediate_drawer_.get());
  // Last, so the Guide draws over the game's own overlays.
  if (REXCVAR_GET(kameo_rexliveless) && kameo_immediate_drawer_) {
    rexglue_guide::Install(this, drawer, {
        .title_name = "Kameo: Elements of Power",
        .on_quit = [this] { if (auto* w = window()) w->RequestClose(); },
        .immediate_drawer = kameo_immediate_drawer_.get(),
    });
    kameo_guide_installed_ = true;
  }
}

// The menu's own settings file loads after the main config so its values win, and before setup reads any of them.
void KameoApp::OnPostInitLogging() {
  // ReXApp keeps its capture sink private, so the console overlay is given one registered here.
  kameo_log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(kameo_log_sink_);
  // As early as a crash can be logged: a report from another machine otherwise ends at the last ordinary line.
  InstallCrashReporter();
#ifdef _WIN32
  // plume reports a refused resource creation on stderr only, which a windowed app has nowhere to show; it lands beside the log instead.
  {
    const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "logs" / "stderr.txt";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    FILE* redirected = nullptr;
    if (freopen_s(&redirected, path.string().c_str(), "a", stderr) == 0 && redirected != nullptr) setvbuf(stderr, nullptr, _IONBF, 0);
  }
#endif
  // The first thing a report needs when the game cannot find its files.
  REXLOG_INFO("game data folder: {}", game_data_root().string());
  if (!kameo_paths_note_.empty()) REXLOG_WARN("{}", kameo_paths_note_);
  KameoWaitForReplacedProcess();
  KameoLoadSettings();
  // After the settings, which hold crash_reports, and before the runtime, whose exception handler has to follow Sentry's. It stays off in builds without a DSN.
  sentrywork::Init({.app_name = "kameorepowered", .tags = {{"game", "Kameo: Elements of Power"}}});
  InstallCrashReporter();
}

// The game data is the assets folder next to the executable unless game_data_root names another, on the command line or in kameorepowered.toml; a wrong game_data_root fails with its path rather than being swapped for some other folder.
void KameoApp::OnConfigurePaths(rex::PathConfig& paths) {
  namespace fs = std::filesystem;
  kameo_config_path_ = paths.config_path;
  // The SDK resolves paths before it loads kameorepowered.toml, so a root set only there (as the launcher expects) is applied here.
  if (REXCVAR_GET(user_data_root).empty()) {
    if (const std::string root = ConfiguredPath(paths.config_path, "user_data_root"); !root.empty()) {
      paths.user_data_root = root;
      if (REXCVAR_GET(cache_root).empty()) paths.cache_root = paths.user_data_root / "cache";
    }
  }
  if (paths.game_data_root.empty()) {
    if (const std::string root = ConfiguredPath(paths.config_path, "game_data_root"); !root.empty()) paths.game_data_root = root;
  }
  if (paths.game_data_root.empty()) {
    paths.game_data_root = rex::filesystem::GetExecutableFolder() / "assets";
  }
  // A disc extracted one folder deeper than asked (assets\<disc name>\default.xex) is taken as it is when it is the only candidate.
  std::error_code ec;
  if (!fs::exists(paths.game_data_root / "default.xex", ec) && fs::is_directory(paths.game_data_root, ec)) {
    fs::path nested;
    int candidates = 0;
    for (const auto& entry : fs::directory_iterator(paths.game_data_root, ec)) {
      if (entry.is_directory(ec) && fs::exists(entry.path() / "default.xex", ec)) {
        nested = entry.path();
        ++candidates;
      }
    }
    if (candidates == 1) {
      kameo_paths_note_ = fmt::format("game data found one folder down, in {}; the expected layout is default.xex directly inside {}", nested.string(), paths.game_data_root.string());
      paths.game_data_root = nested;
    }
  }
}
