// The game's app: the native renderer, our ImGui windows over the SDK's overlays, the language redirect and the guest FP-exception guard.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/app/title_app.h>
#include <rex/utility.h>

namespace rex::filesystem { class IFileSystem; }
namespace kameo::gpu {
class GraphicsSystem;
class OverlayRenderer;
}  // namespace kameo::gpu

class KameoDialog;

namespace kameo::app {

#ifdef _WIN32
// Without this the window is sized in logical pixels and Windows bitmap-stretches it, which made the game soft on a scaled display.
void DeclarePerMonitorDpiAwareness();
#endif
// The user language's XLanguage id, or 1 (English) when it is missing or not a shipped language.
uint32_t ResolveUserLanguage();
// Builds the merged language folder the "english" link points at; must run before the game opens its data.
void BuildMergedLanguageDir(const std::filesystem::path& game_root);
// Points the game's hardcoded "english" folders, base game and DLC, at the chosen language.
void RegisterLanguageRedirects(rex::filesystem::IFileSystem& vfs, uint32_t lang);

}  // namespace kameo::app

class KameoApp : public rex::app::TitleApp {
 public:
  KameoApp();
  ~KameoApp() override;

  // Drops what is registered with the loaded title (the achievement listeners, ours and the Guide's) while the title still exists. Called once the frame loop has ended, before Shutdown.
  void ReleaseTitleListeners();

 protected:
  rex::AppSysRequirements GetRequirements() const override;
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override;
  rex::Result<void> OnInit() override;
  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override;
  void OnShutdown() override;

 private:
  // Whether the native renderer runs this session, decided once the settings are final.
  bool UseNativeRenderer() const;
  void ResolveGameFolders();
  rex::Result<void> StartNativeRenderer();
  void CreateDialogs();

  void* veh_handle_ = nullptr;
  mutable int native_renderer_ = -1;  // -1 until decided
  std::unique_ptr<kameo::gpu::GraphicsSystem> kameo_graphics_;
  std::unique_ptr<kameo::gpu::OverlayRenderer> kameo_overlay_renderer_;
  // Our ImGui windows, in draw order.
  std::unique_ptr<KameoDialog> kameo_name_entry_keys_;
  std::unique_ptr<KameoDialog> kameo_fps_overlay_;
  std::unique_ptr<KameoDialog> kameo_loading_overlay_;
  std::unique_ptr<KameoDialog> kameo_achievement_icons_;
  std::unique_ptr<KameoDialog> kameo_pause_card_;
  std::unique_ptr<KameoDialog> kameo_hotkeys_;
  // Unlock notices go to the game's own text box; this is the manager's callback registration.
  rex::ScopeGuard kameo_achievement_listener_;
  // The RexLiveless Guide (kameo.rexliveless), installed with the dialogs.
  bool kameo_guide_installed_ = false;
};
