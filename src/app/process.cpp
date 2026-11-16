// Restarting and quitting the game from its menus.

#include "app/process.h"

#include <atomic>

#include <rex/app/app.h>
#include <rex/logging/log_category.h>

namespace {

// Set once a restart or quit is under way, so a second press while the game closes neither starts another copy nor turns a quit into a restart.
std::atomic<bool> g_closing{false};

}  // namespace

void KameoQuitApplication() {
  if (g_closing.exchange(true)) return;
  REXLOG_INFO("[kameo] quit: closing the game from the pause menu");
  rex::app::RequestShutdown(0);
}

// The app starts the new copy itself once this one has shut its systems down, so the two never hold the audio device or the game's files at once.
void KameoRestartApplication() {
  if (g_closing.exchange(true)) return;
  REXLOG_INFO("[kameo] restart: closing this copy and starting a new one");
  rex::app::RequestRestart();
}
