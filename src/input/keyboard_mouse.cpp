// The controller poll: each player's controller remapped, or their keyboard and mouse turned into a controller state.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"
#include "ui/pause_menu/pause_menu.h"

REXCVAR_DECLARE(double, mnk_sensitivity);
REXCVAR_DECLARE(bool, kameo_story_splitscreen);
REXCVAR_DECLARE(int32_t, kameo_input_mode);

namespace kameo::input::detail {

using rex::X_RESULT;
using rex::X_STATUS;

rex::input::mnk::MnkInputDriver* g_keyboard = nullptr;
std::atomic<uint32_t> g_standbyPadButtons{0};
std::array<uint32_t, 2> g_padPolls{};
std::array<uint32_t, 2> g_mouseLookPoll{};

namespace {

// The turn rate a full stick is taken to give for aims read from the right stick, set below Chilla's 3 radians a second since the game eases the rate in and out.
constexpr float kAimTurnRate = 1.5f;
// xfix_thunk_ClampStickLikePADClamp_820EE080 scales the raw right stick by this, drops the first 15 on each axis and keeps up to 59 more.
constexpr float kStickRawScale = 0.0030212402f;
constexpr float kStickDeadZone = 15.0f;
constexpr float kStickTravel = 59.0f;
constexpr int16_t kPadUseThreshold = 16000;

std::array<int32_t, 2> g_triggerPolls{};
std::array<int32_t, 2> g_triggerSide{};
// Player one's state as last handed to the game, for a pause menu player two opened and then left without a controller of their own.
rex::input::X_INPUT_STATE g_playerOneOutput{};

// The raw stick value that comes out of the game's dead zone as this share of full deflection.
int16_t MouseStickAxis(float share) {
  const float units = std::min(std::abs(share), 1.0f) * kStickTravel;
  if (units < 1.0f) return 0;
  const float raw = (kStickDeadZone + 0.5f + units) / kStickRawScale;
  return int16_t(share < 0.0f ? -raw : raw);
}

}  // namespace

}  // namespace kameo::input::detail

using namespace kameo::input;
using namespace kameo::input::detail;

uint32_t KameoStandbyPadButtons() { return g_standbyPadButtons.load(std::memory_order_relaxed); }

// XInputGetState(user, state) -- controllerTick2's only poll; the assigned keyboard player's state replaces the runtime state while mouse look is applied separately.
extern "C" REX_FUNC(sub_826BA2F0) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  if (user == 1 && state && KameoPausedBy() == 1 && !CoopInputRouteActive()) {
    std::memcpy(base + state, &g_playerOneOutput, sizeof(g_playerOneOutput));
    ctx.r3.u64 = 0;
    return;
  }
  struct KeepPlayerOne {
    PPCContext& ctx;
    uint8_t* base;
    uint32_t user;
    uint32_t state;
    ~KeepPlayerOne() {
      if (user == 0 && state && ctx.r3.u32 == 0) std::memcpy(&g_playerOneOutput, base + state, sizeof(g_playerOneOutput));
    }
  } keep_player_one{ctx, base, user, state};
  // The controller poll keeps running while the game is paused, unlike the per-tick input update, so a mode switched in the pause menu takes effect here at once.
  SyncInputMode();
  __imp__sub_826BA2F0(ctx, base);
  if (!state) return;
  rex::input::X_INPUT_STATE physical{};
  if (ctx.r3.u32 == 0) std::memcpy(&physical, base + state, sizeof(physical));
  if (user < 2) {
    // A newly pressed button, or a stick or trigger pushed past the threshold; one resting at the end of its range does not count again.
    static std::array<uint16_t, 2> held{};
    static std::array<bool, 2> pushed{};
    const auto& pad = physical.gamepad;
    const bool push = std::abs(int32_t(pad.thumb_lx)) > kPadUseThreshold || std::abs(int32_t(pad.thumb_ly)) > kPadUseThreshold || std::abs(int32_t(pad.thumb_rx)) > kPadUseThreshold || std::abs(int32_t(pad.thumb_ry)) > kPadUseThreshold || pad.left_trigger > 64 || pad.right_trigger > 64;
    if (ctx.r3.u32 == 0 && ((uint16_t(pad.buttons) & ~held[user]) || (push && !pushed[user]))) g_padUsed[user].store(KameoSteadyMilliseconds(), std::memory_order_relaxed);
    held[user] = uint16_t(pad.buttons);
    pushed[user] = push;
  }
  if (user == 0) ChooseControlsByUse();
  if (user == 0) g_standbyPadButtons.store(REXCVAR_GET(kameo_input_mode) != 1 && !REXCVAR_GET(kameo_story_splitscreen) ? uint16_t(RemapPad(physical.gamepad).buttons) : 0, std::memory_order_relaxed);
  if (user == 0 && ctx.r3.u32 == 0 && FeedPadCapture(physical.gamepad)) {
    rex::input::X_INPUT_STATE neutral{};
    std::memcpy(base + state, &neutral, sizeof(neutral));
    return;
  }
  if (!KameoPlayerUsesKeyboard(int(user))) {
    if (ctx.r3.u32 != 0) return;
    physical.gamepad = RemapPad(physical.gamepad);
    SuppressPlayerOneActivation(user, &physical.gamepad);
    std::memcpy(base + state, &physical, sizeof(physical));
    return;
  }
  rex::input::X_INPUT_STATE keyboard{};
  rex::input::X_INPUT_STATE aggregate{};
  bool have_aggregate = false;
  if (g_keyboard) {
    have_aggregate = g_keyboard->GetDeviceState(kKeyboardDevice, &aggregate) == X_ERROR_SUCCESS;
  }
  rex::input::X_INPUT_STATE device_keyboard{};
  const bool have_device_keyboard = ReadPhysicalKeyboardState(int(user), &device_keyboard);
  if (have_aggregate && !(KameoPlayerUsesKeyboard(0) && KameoPlayerUsesKeyboard(1))) {
    keyboard = aggregate;
    if (have_device_keyboard) {
      keyboard.gamepad.buttons = uint16_t(keyboard.gamepad.buttons) | uint16_t(device_keyboard.gamepad.buttons);
      keyboard.gamepad.left_trigger = std::max(uint8_t(keyboard.gamepad.left_trigger), uint8_t(device_keyboard.gamepad.left_trigger));
      keyboard.gamepad.right_trigger = std::max(uint8_t(keyboard.gamepad.right_trigger), uint8_t(device_keyboard.gamepad.right_trigger));
      if (keyboard.gamepad.thumb_lx == 0) keyboard.gamepad.thumb_lx = device_keyboard.gamepad.thumb_lx;
      if (keyboard.gamepad.thumb_ly == 0) keyboard.gamepad.thumb_ly = device_keyboard.gamepad.thumb_ly;
      if (keyboard.gamepad.thumb_rx == 0) keyboard.gamepad.thumb_rx = device_keyboard.gamepad.thumb_rx;
      if (keyboard.gamepad.thumb_ry == 0) keyboard.gamepad.thumb_ry = device_keyboard.gamepad.thumb_ry;
    }
  } else if (have_device_keyboard) keyboard = device_keyboard;
  else return;
  const size_t player = size_t(user);
  ++g_padPolls[player];
  if (g_mouseCaptured) {
    // The camera keys push the stick to the rim; anything short of it is mouse travel.
    if (std::abs(int32_t(keyboard.gamepad.thumb_rx)) < 32000) keyboard.gamepad.thumb_rx = 0;
    if (std::abs(int32_t(keyboard.gamepad.thumb_ry)) < 32000) keyboard.gamepad.thumb_ry = 0;
    // Aiming warriors and the aim cameras read the right stick themselves instead of cameraDefaultJoyProcess, so while it is not taking the mouse the travel is handed to the stick.
    if (g_mouseLook && g_padPolls[player] - g_mouseLookPoll[player] > 2) {
      float dx = 0.0f;
      float dy = 0.0f;
      TakePhysicalMouseTravel(int(user), &dx, &dy);
      const float tick = std::clamp(LoadFloat(base, kFrameTime), 1.0f / 240.0f, 0.1f);
      const float scale = kRadiansPerPixel * float(REXCVAR_GET(mnk_sensitivity)) / (kAimTurnRate * tick);
      if (keyboard.gamepad.thumb_rx == 0) keyboard.gamepad.thumb_rx = MouseStickAxis(dx * scale);
      if (keyboard.gamepad.thumb_ry == 0) keyboard.gamepad.thumb_ry = MouseStickAxis(-dy * scale);
    }
  }
  if (!g_mouseLook) {
    if (user == 0 && !REXCVAR_GET(kameo_story_splitscreen)) {
      const auto pad = RemapPad(physical.gamepad);
      keyboard.gamepad.buttons = uint16_t(keyboard.gamepad.buttons) | uint16_t(pad.buttons);
      keyboard.gamepad.left_trigger = std::max(keyboard.gamepad.left_trigger, pad.left_trigger);
      keyboard.gamepad.right_trigger = std::max(keyboard.gamepad.right_trigger, pad.right_trigger);
      if (keyboard.gamepad.thumb_lx == 0) keyboard.gamepad.thumb_lx = pad.thumb_lx;
      if (keyboard.gamepad.thumb_ly == 0) keyboard.gamepad.thumb_ly = pad.thumb_ly;
    }
    // In a menu the mouse buttons are A and B, and the triggers they are bound to for play stay released.
    bool left = false;
    bool right = false;
    if (KameoPlayerUsesKeyboard(0) && KameoPlayerUsesKeyboard(1)) {
      std::lock_guard<std::mutex> lock(g_physicalInputMutex);
      left = g_playerMice[player].left;
      right = g_playerMice[player].right;
    } else {
      std::lock_guard<std::mutex> lock(g_mouse.mutex);
      left = g_mouse.left_down;
      right = g_mouse.right_down;
    }
    // Each wheel notch is one trigger press, up for the left trigger and down for the right, held a few polls and released a few before the next so the menu sees every edge.
    if (g_triggerPolls[player] == 0) {
      const int32_t notch = rex::ui::MouseEvent::kScrollPerDetent;
      if (KameoPlayerUsesKeyboard(0) && KameoPlayerUsesKeyboard(1)) {
        std::lock_guard<std::mutex> lock(g_physicalInputMutex);
        if (g_playerMice[player].wheel >= notch || g_playerMice[player].wheel <= -notch) {
          g_triggerSide[player] = g_playerMice[player].wheel > 0 ? -1 : 1;
          g_playerMice[player].wheel -= g_playerMice[player].wheel > 0 ? notch : -notch;
          g_triggerPolls[player] = 8;
        }
      } else {
        std::lock_guard<std::mutex> lock(g_mouse.mutex);
        if (g_mouse.trigger_wheel >= notch || g_mouse.trigger_wheel <= -notch) {
          g_triggerSide[player] = g_mouse.trigger_wheel > 0 ? -1 : 1;
          g_mouse.trigger_wheel -= g_mouse.trigger_wheel > 0 ? notch : -notch;
          g_triggerPolls[player] = 8;
        }
      }
    }
    if (g_triggerPolls[player] > 0) --g_triggerPolls[player];
    const bool trigger_held = g_triggerPolls[player] > 4;
    uint16_t buttons = keyboard.gamepad.buttons;
    if (left) buttons |= rex::input::X_INPUT_GAMEPAD_A;
    // "Press Start" has no menu to click, so there the left button is Start as well.
    if (left && AtTitleScreen()) buttons |= rex::input::X_INPUT_GAMEPAD_START;
    if (right) buttons |= rex::input::X_INPUT_GAMEPAD_B;
    keyboard.gamepad.buttons = buttons;
    if (left || right) {
      keyboard.gamepad.left_trigger = 0;
      keyboard.gamepad.right_trigger = 0;
    }
    if (trigger_held) {
      if (g_triggerSide[player] < 0) keyboard.gamepad.left_trigger = 255;
      else keyboard.gamepad.right_trigger = 255;
    }
  }
  if (user == 1) TraceP2KeyboardPoll(base, have_aggregate, have_device_keyboard, aggregate, device_keyboard, keyboard);
  SuppressPlayerOneActivation(user, &keyboard.gamepad);
  std::memcpy(base + state, &keyboard, sizeof(keyboard));
  ctx.r3.u64 = 0;
}
