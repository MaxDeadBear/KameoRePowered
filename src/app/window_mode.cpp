// Window mode: windowed, borderless or exclusive fullscreen, switched live through the runtime's own window on the UI thread, and the marker that catches a crashed exclusive fullscreen session.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "app/process.h"
#include "app/window_mode.h"
#include "core/settings.h"
#include "gpu/graphics_system.h"

REXCVAR_DEFINE_INT32(kameo_window_mode, -1, "Kameo", "0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen (native renderer on D3D12); -1 follows the fullscreen setting.");

namespace {

// Present only while the game runs in exclusive fullscreen, so one left behind means that session never shut down cleanly.
constexpr const char* kFullscreenMarker = "kameo_fullscreen.running";

std::filesystem::path FullscreenMarker() { return rex::filesystem::GetExecutableFolder() / kFullscreenMarker; }

kameo::gpu::GraphicsSystem* Graphics() { return kameo::gpu::GraphicsSystem::instance(); }

void SetWindowed(rex::ui::Window* window) {
  window->SetFullscreen(false);
  REXLOG_INFO("[kameo] window mode windowed");
}

// Leaving exclusive fullscreen happens on the render thread, so the window only changes size once that has taken effect; DXGI's SetFullscreenState needs this window's thread pumping messages to finish, so the wait runs on a thread of its own rather than blocking the UI thread into a stand-off that ends with the window resized mid-transition.
void ApplyWindowed(rex::ui::Window* window) {
  if (auto* graphics = Graphics(); graphics && graphics->ExclusiveFullscreenActive()) {
    std::thread([window] {
      for (int attempt = 0; attempt < 400; ++attempt) {
        auto* current = Graphics();
        if (!current || !current->ExclusiveFullscreenActive()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      window->app_context().CallInUIThreadDeferred([window] { SetWindowed(window); });
    }).detach();
    return;
  }
  SetWindowed(window);
}

}  // namespace

int32_t KameoWindowMode() {
  const int32_t mode = REXCVAR_GET(kameo_window_mode);
  if (mode >= 0 && mode <= 2) return mode;
  return REXCVAR_GET(fullscreen) ? 1 : 0;
}

void KameoInitWindowMode(rex::ui::Window* window) {
  kameo::app::SetMainWindow(window);
  if (window && REXCVAR_GET(kameo_window_mode) >= 0) KameoApplyWindowMode();
}

void KameoSetWindowMode(int32_t mode) {
  REXCVAR_SET(kameo_window_mode, mode);
  KameoApplyWindowMode();
}

// The exe links its own copy of SDL that has no windows, so the switch goes through the runtime's Window rather than SDL directly.
void KameoApplyWindowMode() {
  rex::ui::Window* window = kameo::app::MainWindow();
  if (!window) return;
  const int32_t mode = KameoWindowMode();
  KameoNoteExclusiveFullscreen(mode == 2);
  if (auto* graphics = Graphics(); graphics && mode != 2) graphics->SetExclusiveFullscreen(false);
  window->app_context().CallInUIThreadDeferred([window, mode] {
    if (mode == 0) {
      ApplyWindowed(window);
      return;
    }
    // Exclusive fullscreen starts from a borderless window already the display's size, so entering it changes no swap chain dimensions.
    window->SetFullscreen(true);
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
  rex::cvar::SetFlagByName("kameo_window_mode", "1");
  KameoSaveSettings();
}
