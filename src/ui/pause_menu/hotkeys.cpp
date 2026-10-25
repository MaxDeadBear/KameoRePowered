// F4 and F6: the Options and Achievements pages, reached through the game's own pause menu rather than an ImGui window.

#include "ui/pause_menu/pause_menu.h"

#include <atomic>

#include "input/guest_controls.h"
#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;
using kameo::input::kPlayerList;

namespace {

std::atomic<int> g_requested{-1};
// Set when the menu has to be opened first; xfixgem_storybook_initSimplePauseMenuWidget's hook takes it and shows the game a Start press.
bool g_openPause = false;

// The open simple pause (or main-menu options) object, else 0.
constexpr uint32_t kSimplePauseObject = 0x82884884;
constexpr uint32_t kStoredPauseValue = 0x82B71904;
constexpr uint32_t kSimplePauseBit = 0x10;

}  // namespace

namespace kameo::ui {

bool TakeHotkeyPauseOpen() {
  const bool open = g_openPause;
  g_openPause = false;
  return open;
}

}  // namespace kameo::ui

void KameoRequestMenuPage(KameoMenuHotkey which) { g_requested.store(int(which), std::memory_order_relaxed); }

void KameoTickMenuHotkeys(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  const int which = g_requested.exchange(-1, std::memory_order_relaxed);
  if (which < 0) return;
  const Page page = which == int(KameoMenuHotkey::Achievements) ? Page::Achievements : Page::Options;
  // Already open: the pause object's tick switches it to the page.
  if (REX_LOAD_U32(kSimplePauseObject) != 0) {
    g_hotkeyPage = page;
    return;
  }
  // No player means the main menu, a load or a movie, none of which has a pause menu to open.
  if (REX_LOAD_U32(kPlayerList) == 0) return;
  if ((REX_LOAD_U32(kStoredPauseValue) & kSimplePauseBit) != 0) return;
  // The game opens its pause menu only on a Start press it reads itself, so the hook that runs its pause handling shows it one.
  g_openPause = true;
  g_hotkeyPage = page;
}
