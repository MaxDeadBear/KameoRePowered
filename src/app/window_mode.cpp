// Window mode: windowed, borderless or exclusive fullscreen, switched live through the UI system's fullscreen setting, and the marker that catches a crashed exclusive fullscreen session.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#include <rex/logging/log_category.h>
#include <rex/module.h>
#include <rex/platform/file.h>
#include <rex/ui/ui_system.h>

#include "app/process.h"
#include "app/window_mode.h"
#include "core/cvar.h"
#include "core/settings.h"
#include "gpu/graphics_system.h"

KCVAR_DEFINE_INT32(kameo_window_mode, -1, "Kameo", "0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen (native renderer on D3D12); -1 follows the fullscreen setting.");

namespace {

// Present only while the game runs in exclusive fullscreen, so one left behind means that session never shut down cleanly.
constexpr const char* kFullscreenMarker = "kameo_fullscreen.running";

std::filesystem::path FullscreenMarker() { return rex::platform::GetExecutableFolder() / kFullscreenMarker; }

kameo::gpu::GraphicsSystem* Graphics() { return kameo::gpu::GraphicsSystem::instance(); }

rex::ui::IUISystem* UI() { return REX_INTERFACE(rex::ui::IUISystem); }

// The UI system applies a change to its fullscreen setting to the window on the UI thread.
void SetFullscreen(bool fullscreen) {
  if (rex::ui::IUISystem* ui = UI()) kameo::cvar::Store(ui->GetSettings().fullscreen, fullscreen);
}

void SetWindowed() {
  SetFullscreen(false);
  REXLOG_INFO("[kameo] window mode windowed");
}

// Leaving exclusive fullscreen happens on the render thread, so the window only changes size once that has taken effect; DXGI's SetFullscreenState needs the window's thread pumping messages to finish, so the wait runs on a thread of its own rather than blocking the UI thread into a stand-off that ends with the window resized mid-transition.
void ApplyWindowed() {
  if (auto* graphics = Graphics(); graphics && graphics->ExclusiveFullscreenActive()) {
    std::thread([] {
      for (int attempt = 0; attempt < 400; ++attempt) {
        auto* current = Graphics();
        if (!current || !current->ExclusiveFullscreenActive()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (rex::ui::IUISystem* ui = UI()) ui->PostTask([] { SetWindowed(); });
    }).detach();
    return;
  }
  SetWindowed();
}

}  // namespace

int32_t KameoWindowMode() {
  const int32_t mode = KCVAR_GET(kameo_window_mode);
  if (mode >= 0 && mode <= 2) return mode;
  return kameo::cvar::GetBool("fullscreen", true) ? 1 : 0;
}

void KameoInitWindowMode() {
  if (KCVAR_GET(kameo_window_mode) >= 0) KameoApplyWindowMode();
}

void KameoSetWindowMode(int32_t mode) {
  KCVAR_SET(kameo_window_mode, mode);
  KameoApplyWindowMode();
}

void KameoApplyWindowMode() {
  rex::ui::IUISystem* ui = UI();
  if (!ui || !ui->HasWindow()) return;
  const int32_t mode = KameoWindowMode();
  KameoNoteExclusiveFullscreen(mode == 2);
  if (auto* graphics = Graphics(); graphics && mode != 2) graphics->SetExclusiveFullscreen(false);
  ui->PostTask([mode] {
    if (mode == 0) {
      ApplyWindowed();
      return;
    }
    // Exclusive fullscreen starts from a borderless window already the display's size, so entering it changes no swap chain dimensions.
    SetFullscreen(true);
    if (auto* graphics = Graphics(); graphics && mode == 2) graphics->SetExclusiveFullscreen(true);
    REXLOG_INFO("[kameo] window mode {}", mode == 2 ? "exclusive fullscreen" : "borderless fullscreen");
  });
}

void KameoNoteExclusiveFullscreen(bool active) {
  const std::filesystem::path marker = FullscreenMarker();
  std::error_code ec;
  if (active) {
    std::ofstream(marker).put('\n');
  } else {
    std::filesystem::remove(marker, ec);
  }
}

void kameo::app::RecoverFromCrashedFullscreen() {
  const std::filesystem::path marker = FullscreenMarker();
  std::error_code ec;
  if (!std::filesystem::exists(marker, ec)) return;
  std::filesystem::remove(marker, ec);
  if (KameoWindowMode() != 2) return;
  REXLOG_WARN("[kameo] the last exclusive fullscreen session did not shut down cleanly; starting borderless instead");
  kameo::cvar::SetText("kameo_window_mode", "1");
  KameoSaveSettings();
}
