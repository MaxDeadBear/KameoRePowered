// The game's ReXApp: the native renderer, the ImGui overlay stack, the language redirect and the guest FP-exception guard.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/logging/sink.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "gpu/graphics_system.h"
#include "gpu/ui/imgui_drawer.h"
#include "ui/hud/achievement_icons.h"
#include "ui/hud/fps_overlay.h"
#include "ui/hud/loading_overlay.h"
#include "ui/name_entry/name_entry_keys.h"

namespace rex::filesystem { class VirtualFileSystem; }

namespace kameo::app {

#ifdef _WIN32
// Without this the window is sized in logical pixels and Windows bitmap-stretches it, which made the game soft on a scaled display.
void DeclarePerMonitorDpiAwareness();
#endif
// The --user_language XLanguage id, or 1 (English) when it is missing or not a shipped language.
uint32_t ResolveUserLanguage();
// Builds the merged language folder the "english" symlink points at; must run before the runtime mounts the game data.
void BuildMergedLanguageDir(const std::filesystem::path& game_root);
// Points the game's hardcoded "english" folders, base game and DLC, at the chosen language.
void RegisterLanguageRedirects(rex::filesystem::VirtualFileSystem& vfs, uint32_t lang);

}  // namespace kameo::app

class KameoApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

  void OnPreSetup(rex::RuntimeConfig& config) override;
  bool SetupPresentation() override;
  void OnShutdown() override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnPostInitLogging() override;
  void OnConfigurePaths(rex::PathConfig& paths) override;

 private:
  void* veh_handle_ = nullptr;
  std::unique_ptr<KameoNameEntryKeys> kameo_name_entry_keys_;
  std::unique_ptr<KameoFpsOverlay> kameo_fps_overlay_;
  std::unique_ptr<KameoLoadingOverlay> kameo_loading_overlay_;
  std::unique_ptr<KameoAchievementIcons> kameo_achievement_icons_;
  kameo::gpu::GraphicsSystem* kameo_graphics_ = nullptr;
  std::unique_ptr<kameo::gpu::PlumeImmediateDrawer> kameo_immediate_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> kameo_imgui_drawer_;
  std::shared_ptr<rex::LogCaptureSink> kameo_log_sink_;
  std::filesystem::path kameo_config_path_;
  std::string kameo_paths_note_;  // what OnConfigurePaths wants said once logging exists
  std::unique_ptr<rex::ui::ImGuiDialog> kameo_console_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> kameo_debug_overlay_;
  // Unlock notices go to the game's own text box; this is the manager's callback registration.
  uint64_t kameo_achievement_listener_ = 0;
  // The RexLiveless Guide (kameo_rexliveless), installed with the dialogs.
  bool kameo_guide_installed_ = false;
};
