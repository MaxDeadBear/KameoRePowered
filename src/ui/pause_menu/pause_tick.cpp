// The pause object's tick: who paused, finished key and controller captures, X and Y on the quit prompt, and left and right on an option.

#include <algorithm>
#include <atomic>
#include <string>

#include "app/process.h"
#include "core/settings.h"
#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "ui/menu_pointer/menu_ui_state.h"
#include "ui/pause_menu/pause_menu.h"
#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;
using namespace kameo::input;

namespace {

std::atomic<int> g_pauseOwner{-1};
std::atomic<int64_t> g_pauseOwnerSeen{0};

// The player who owns a pause object, from its header.
int8_t PausePlayer(uint8_t* base, uint32_t pause) { return int8_t(REX_LOAD_U8(REX_LOAD_U32(pause + 8) + 146)); }

// A finished capture saves the bindings and rebuilds its page so the row shows the new key.
void RefreshBindingPage(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, Page page) {
  KameoSaveSettings();
  if (!window || g_currentPage != page) return;
  const uint32_t index = SelectedIndex(base, window);
  ActivatePage(ctx, base, pause, window, page);
  KeepCursor(ctx, base, window, index);
}

}  // namespace

// The player whose pause menu is open, or -1: the menu takes input only from them.
int KameoPausedBy() { return KameoSteadyMilliseconds() - g_pauseOwnerSeen.load(std::memory_order_relaxed) < 200 ? g_pauseOwner.load(std::memory_order_relaxed) : -1; }

// storybookSimplePauseObj_tick(pauseObj) -- left and right step the selected option, read the way processGammaBar reads the gamma bar, and X on the quit prompt restarts the game.
extern "C" REX_FUNC(sub_823EFB78) {
  const uint32_t pause = ctx.r3.u32;
  if (pause && REX_LOAD_U32(pause + 8)) {
    g_pauseOwner.store(std::clamp<int>(PausePlayer(base, pause), 0, 3), std::memory_order_relaxed);
    g_pauseOwnerSeen.store(KameoSteadyMilliseconds(), std::memory_order_relaxed);
  }
  __imp__sub_823EFB78(ctx, base);
  static int held_direction = 0;
  static float repeat_timer = 0.0f;
  const uint32_t record = pause ? REX_LOAD_U32(pause + 512) : 0;
  const uint32_t window = record ? REX_LOAD_U32(record + 68) : 0;
  NotePauseMenuWindow(window);
  std::string bound_cvar;
  std::string bound_key;
  bool bind_cancelled = false;
  if (KameoTakeCapturedKey(bound_cvar, bound_key, bind_cancelled)) RefreshBindingPage(ctx, base, pause, window, Page::Bindings);
  if (KameoTakeCapturedPad(bind_cancelled)) RefreshBindingPage(ctx, base, pause, window, Page::PadBindings);
  // The quit prompt is the page whose B handler is storybookSimplePauseQuitMenuPressBCallback.
  if (window && REX_LOAD_U32(MenuOf(base, TextObjectOf(base, window)) + kMenuBackCallback) == kQuitBackCallback) {
    const uint32_t pressed = REX_LOAD_U32(PlayerStick(base, PausePlayer(base, pause)) + kStickPressed) & REX_LOAD_U32(kCommitTestButtonMask);
    // With keyboard and mouse the prompt has its own Restart Game and Quit Application lines, and X and Y sit on everyday keys (Q, E).
    if (KameoKeyboardMouseMode()) return;
    if ((pressed & kButtonX) != 0) {
      KameoSaveSettings();
      KameoRestartApplication();
    } else if ((pressed & kButtonY) != 0) {
      KameoSaveSettings();
      KameoQuitApplication();
    }
    return;
  }
  const uint32_t page_id = record ? REX_LOAD_U32(record + 120) : 0;
  const Option* option = window && (page_id == 1 || page_id == 2) ? FindOption(SelectedItemId(base, window, SelectedIndex(base, window))) : nullptr;
  // Binding rows only change through A, which waits for a key.
  if (option && (option->page == Page::Bindings || option->page == Page::PadBindings)) option = nullptr;
  int direction = 0;
  if (option) {
    const uint32_t stick = PlayerStick(base, PausePlayer(base, pause));
    const uint32_t held = REX_LOAD_U32(stick + kStickHeld) & REX_LOAD_U32(kCommitTestButtonMask);
    const float x = LoadFloat(base, stick + kStickLeftX);
    if (x > 0.5f || (held & 0x20000008) != 0) direction = 1;
    if (x < -0.5f || (held & 0x10000004) != 0) direction = -1;
  }
  if (direction == 0) {
    held_direction = 0;
    return;
  }
  // First press acts at once, then repeats after 0.4 s every 0.12 s while held.
  if (direction != held_direction) {
    held_direction = direction;
    repeat_timer = 0.4f;
  } else {
    repeat_timer -= LoadFloat(base, kFrameTime);
    if (repeat_timer > 0.0f) return;
    repeat_timer = 0.12f;
  }
  ChangeOption(ctx, base, pause, window, *option, direction, false);
}
