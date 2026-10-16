// Which player uses the keyboard and mouse: the input mode settings, player two joining co-op, and controls following whichever device each player touches.

#include <algorithm>
#include <atomic>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"

REXCVAR_DECLARE(bool, kameo_story_splitscreen);
REXCVAR_DECLARE(int32_t, kameo_input_mode);
REXCVAR_DECLARE(int32_t, kameo_p2_input_mode);

namespace kameo::input::detail {

std::atomic<int64_t> g_frontendP2JoinSeen{0};
std::atomic<int64_t> g_keyboardMouseUsed{0};
std::array<std::atomic<int64_t>, 2> g_padUsed{};

namespace {

// Set when player 2 joined from the main menu's co-op screen, cleared when a game starts from the title's Start prompt alone.
std::atomic<bool> g_menuCoop{false};
std::atomic<bool> g_suppressPlayerOneAUntilRelease{false};
int64_t g_playerTwoClaimSince = 0;
bool g_wasSplit = false;
// Player two's claiming press can be the Start that joined them from the main menu, a moment before split-screen turns on.
constexpr int64_t kClaimLookbackMs = 750;

}  // namespace

void SuppressPlayerOneActivation(uint32_t user, rex::input::X_INPUT_GAMEPAD* gamepad) {
  if (user != 0 || !g_suppressPlayerOneAUntilRelease.load(std::memory_order_relaxed)) return;
  const uint16_t buttons = gamepad->buttons;
  if ((buttons & rex::input::X_INPUT_GAMEPAD_A) == 0) g_suppressPlayerOneAUntilRelease.store(false, std::memory_order_relaxed);
  else gamepad->buttons = buttons & ~rex::input::X_INPUT_GAMEPAD_A;
}

// Alone, player one follows whichever device was pressed last (so "Press Start" chooses it); with player two on, player one keeps theirs and player two claims what they press.
void ChooseControlsByUse() {
  const int64_t keys = g_keyboardMouseUsed.load(std::memory_order_relaxed);
  const int64_t pad = g_padUsed[0].load(std::memory_order_relaxed);
  const int64_t other_pad = g_padUsed[1].load(std::memory_order_relaxed);
  if (!REXCVAR_GET(kameo_story_splitscreen)) g_wasSplit = false;
  if (!REXCVAR_GET(kameo_story_splitscreen) || !LevelRunning()) {
    g_playerTwoClaimSince = 0;
    if (!KameoKeyCaptureTarget().empty() || !KameoPadCaptureTarget().empty()) return;
    const bool keyboard = REXCVAR_GET(kameo_input_mode) != 1;
    if (keyboard && pad > keys) {
      REXCVAR_SET(kameo_input_mode, 1);
      REXLOG_INFO("[kameo] player 1 picked up a controller: gamepad");
    } else if (!keyboard && keys > pad) {
      REXCVAR_SET(kameo_input_mode, 0);
      REXLOG_INFO("[kameo] player 1 used the keyboard or mouse: keyboard and mouse");
    }
    return;
  }
  // With one keyboard and one controller the two players always hold opposite devices: whichever changed last keeps its choice and the other takes what is left.
  uint32_t keyboards = 0;
  {
    std::lock_guard<std::mutex> lock(g_physicalInputMutex);
    for (const SDL_KeyboardID id : g_physicalKeyboardIds) keyboards += id != 0;
  }
  if (g_padCount.load(std::memory_order_relaxed) <= 1 && keyboards <= 1) {
    static bool last_one_pad = false;
    static bool last_two_pad = false;
    const bool one_pad = REXCVAR_GET(kameo_input_mode) == 1;
    const bool two_pad = REXCVAR_GET(kameo_p2_input_mode) != 1;
    if (!g_wasSplit || one_pad == two_pad) {
      const bool two_changed = g_wasSplit && two_pad != last_two_pad && one_pad == last_one_pad;
      if (two_changed) REXCVAR_SET(kameo_input_mode, two_pad ? 0 : 1);
      else REXCVAR_SET(kameo_p2_input_mode, one_pad ? 1 : 0);
      REXLOG_INFO("[kameo] one keyboard and one controller: player 1 {}, player 2 {}", REXCVAR_GET(kameo_input_mode) == 1 ? "gamepad" : "keyboard and mouse", REXCVAR_GET(kameo_p2_input_mode) == 1 ? "keyboard and mouse" : "gamepad");
    }
    g_wasSplit = true;
    g_playerTwoClaimSince = 0;
    last_one_pad = REXCVAR_GET(kameo_input_mode) == 1;
    last_two_pad = REXCVAR_GET(kameo_p2_input_mode) != 1;
    return;
  }
  if (!g_wasSplit) {
    g_wasSplit = true;
    if (REXCVAR_GET(kameo_input_mode) == 1) {
      g_playerTwoClaimSince = KameoSteadyMilliseconds() - kClaimLookbackMs;
      REXCVAR_SET(kameo_p2_input_mode, 0);
      REXLOG_INFO("[kameo] player 2 joined: press a button on another controller, or a key or mouse button, to choose controls");
    } else {
      g_playerTwoClaimSince = 0;
      REXCVAR_SET(kameo_p2_input_mode, 0);
      REXLOG_INFO("[kameo] player 2 joined: player 1 has the keyboard and mouse, so player 2 uses a controller");
    }
  }
  if (!g_playerTwoClaimSince) return;
  if (keys > g_playerTwoClaimSince) {
    g_playerTwoClaimSince = 0;
    REXCVAR_SET(kameo_p2_input_mode, 1);
    REXLOG_INFO("[kameo] player 2 chose keyboard and mouse");
  } else if (other_pad > g_playerTwoClaimSince) {
    g_playerTwoClaimSince = 0;
    REXCVAR_SET(kameo_p2_input_mode, 0);
    REXLOG_INFO("[kameo] player 2 chose a controller");
  }
}

}  // namespace kameo::input::detail

using namespace kameo::input;
using namespace kameo::input::detail;

bool KameoPlayerUsesKeyboard(int player) {
  if (player == 0) return REXCVAR_GET(kameo_input_mode) != 1;
  return player == 1 && CoopInputRouteActive() && REXCVAR_GET(kameo_p2_input_mode) == 1;
}

int KameoKeyboardPlayer() { return KameoPlayerUsesKeyboard(0) ? 0 : KameoPlayerUsesKeyboard(1) ? 1 : -1; }

bool KameoKeyboardMouseMode() { return KameoKeyboardPlayer() >= 0; }

int KameoCoopInputMode() {
  if (!REXCVAR_GET(kameo_story_splitscreen)) return 0;
  return REXCVAR_GET(kameo_p2_input_mode) == 1 ? 2 : 1;
}

void KameoSetCoopInputMode(int mode) {
  g_suppressPlayerOneAUntilRelease.store(true, std::memory_order_relaxed);
  if (mode == 0) g_frontendP2JoinSeen.store(0, std::memory_order_relaxed);
  if (mode != 0) REXCVAR_SET(kameo_p2_input_mode, mode == 2 ? 1 : 0);
  REXCVAR_SET(kameo_story_splitscreen, mode != 0);
  REXLOG_INFO("[kameo] co-op input: {} (player 1 {})", mode == 0 ? "off" : mode == 1 ? "gamepad" : "keyboard and mouse", REXCVAR_GET(kameo_input_mode) == 1 ? "gamepad" : "keyboard and mouse");
}

bool KameoMenuCoop() { return g_menuCoop.load(std::memory_order_relaxed); }

void KameoSetCoopEnabled(bool enabled) {
  g_suppressPlayerOneAUntilRelease.store(true, std::memory_order_relaxed);
  if (!enabled) g_frontendP2JoinSeen.store(0, std::memory_order_relaxed);
  REXCVAR_SET(kameo_story_splitscreen, enabled);
  REXLOG_INFO("[kameo] story split-screen: {} (player 1 {})", enabled ? "on" : "off", REXCVAR_GET(kameo_input_mode) == 1 ? "gamepad" : "keyboard and mouse");
}

void KameoSetPlayerOneInputMode(int mode) {
  g_suppressPlayerOneAUntilRelease.store(true, std::memory_order_relaxed);
  REXCVAR_SET(kameo_input_mode, mode == 1 ? 1 : 0);
  REXLOG_INFO("[kameo] player 1 input: {} (co-op {})", mode == 1 ? "gamepad" : "keyboard and mouse", REXCVAR_GET(kameo_story_splitscreen) ? "on" : "off");
}

// controllerFrontendStart(player) -- player two pressing Start on the main menu joins co-op, and each player is routed to the controller slot of their own number.
extern "C" REX_FUNC(sub_822C73D0) {
  const bool player_two = ctx.r3.u32 == 1;
  if (player_two) g_frontendP2JoinSeen.store(KameoSteadyMilliseconds(), std::memory_order_relaxed);
  __imp__sub_822C73D0(ctx, base);
  if (ctx.r3.u32 != 0) g_menuCoop.store(player_two, std::memory_order_relaxed);
  if (player_two && ctx.r3.u32 != 0) {
    const uint8_t first = REX_LOAD_U8(kPlayerControllers);
    const uint8_t second = REX_LOAD_U8(kPlayerControllers + 36);
    for (uint32_t player = 0; player < 2; ++player) {
      for (uint32_t slot = 0; slot < 4; ++slot) {
        const uint32_t address = kPlayerControllers + 36 * slot;
        if (REX_LOAD_U8(address) != player) continue;
        const uint32_t target = kPlayerControllers + 36 * player;
        REX_STORE_U8(address, REX_LOAD_U8(target));
        REX_STORE_U8(target, uint8_t(player));
        break;
      }
    }
    if (first != 0 || second != 1) REXLOG_INFO("[kameo] front-end controller route corrected: player 1 {}->{} player 2 {}->{}", first, REX_LOAD_U8(kPlayerControllers), second, REX_LOAD_U8(kPlayerControllers + 36));
    REXCVAR_SET(kameo_story_splitscreen, true);
    REXLOG_INFO("[kameo] player 2 joined from the main-menu Start prompt using {}", REXCVAR_GET(kameo_p2_input_mode) == 1 ? "keyboard" : "gamepad");
  }
}
