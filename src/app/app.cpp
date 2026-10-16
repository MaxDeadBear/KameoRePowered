#include "kameorepowered_init.h"

#include "app/app.h"

#include <system_error>

#include <toml++/toml.hpp>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/api.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>

#include "app/process.h"
#include "app/window_mode.h"
#include "core/fp_guard.h"
#include "core/settings.h"
#include "engine/audio.h"
#include "engine/dlc/dlc_models.h"
#include "input/input.h"

REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, cache_root);

namespace {

constexpr const char* kEntryXexName = "default.xex";

std::string ConfiguredUserDataRoot(const std::filesystem::path& config_path) {
  try {
    const toml::table table = toml::parse_file(config_path.string());
    if (auto value = table["user_data_root"].value<std::string>()) return *value;
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
    kameo_imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window(), 64);
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
      ToggleOverlay(ImGuiKey_F4, kameo_settings_overlay_, [this] {
        return std::make_unique<rex::ui::SettingsDialog>(kameo_imgui_drawer_.get(),
                                                         kameo_config_path_);
      });
      // ReXApp's CreateAchievementsOverlay() builds against its own immediate drawer, which stays null here.
      ToggleOverlay(ImGuiKey_F6, kameo_achievements_overlay_, [this] {
        return std::make_unique<rex::ui::AchievementsOverlayDialog>(
            kameo_imgui_drawer_.get(), kameo_immediate_drawer_.get(), runtime(),
            &achievements());
      });
    });
  }
  return true;
}

void KameoApp::OnShutdown() {
  KameoNoteExclusiveFullscreen(false);
  if (kameo_graphics_) {
    kameo_graphics_->SetUiFrameCallback(nullptr);
  }
  kameo_fps_overlay_.reset();
  kameo_name_entry_keys_.reset();
  kameo_achievements_overlay_.reset();
  kameo_settings_overlay_.reset();
  kameo_debug_overlay_.reset();
  kameo_console_overlay_.reset();
  if (runtime() && runtime()->imgui_drawer() == kameo_imgui_drawer_.get()) runtime()->set_imgui_drawer(nullptr);
  kameo_imgui_drawer_.reset();
  kameo_immediate_drawer_.reset();
  RemoveGuestFpExceptionHandler(veh_handle_);
  veh_handle_ = nullptr;
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
#ifndef _WIN32
  // Installed after SDK setup so it overrides any SDK-installed SIGFPE handler.
  veh_handle_ = InstallGuestFpExceptionHandlerPosix();
#endif
}

void KameoApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  kameo_name_entry_keys_ = std::make_unique<KameoNameEntryKeys>(drawer);
  kameo_fps_overlay_ = std::make_unique<KameoFpsOverlay>(drawer);
}

// The menu's own settings file loads after the main config so its values win, and before setup reads any of them.
void KameoApp::OnPostInitLogging() {
  // ReXApp keeps its capture sink private, so the console overlay is given one registered here.
  kameo_log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(kameo_log_sink_);
  KameoWaitForReplacedProcess();
  KameoLoadSettings();
}

// Finds the game data next to or above the executable when the configured root does not contain it, so a build runs straight out of its build folder.
void KameoApp::OnConfigurePaths(rex::PathConfig& paths) {
  namespace fs = std::filesystem;
  kameo_config_path_ = paths.config_path;
  // The SDK resolves paths before it loads kameorepowered.toml, so a user_data_root set only there (as the launcher expects) is applied here.
  if (REXCVAR_GET(user_data_root).empty()) {
    if (const std::string root = ConfiguredUserDataRoot(paths.config_path); !root.empty()) {
      paths.user_data_root = root;
      if (REXCVAR_GET(cache_root).empty()) paths.cache_root = paths.user_data_root / "cache";
    }
  }
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
    break;
  }
}
