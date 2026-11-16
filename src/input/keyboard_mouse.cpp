// The controller poll: each player's controller remapped, or their keyboard and mouse turned into a controller state.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/cvar.h"
#if KAMEO_GUIDE
#include <rexglue_guide/guide.h>
#endif

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"
#include "ui/pause_menu/pause_card.h"
#include "ui/pause_menu/pause_menu.h"

#include "input/sdk_settings.h"
KCVAR_DECLARE(bool, kameo_story_splitscreen);
KCVAR_DECLARE(int32_t, kameo_input_mode);

namespace kameo::input::detail {

std::atomic<uint32_t> g_standbyPadButtons{0};
std::array<uint32_t, 2> g_padPolls{};
std::array<uint32_t, 2> g_mouseLookPoll{};

namespace {

// A build without the Guide (KAMEO_GUIDE off) never has it open.
bool GuideIsOpen() {
#if KAMEO_GUIDE
  return rexglue_guide::IsOpen();
#else
  return false;
#endif
}


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
rex::xam::X_INPUT_STATE g_playerOneOutput{};

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
  // The RexLiveless Guide reads the pads from the read this wraps (its XamInputGetState), so the read stays live and only the game is given a pad at rest while the Guide is up; the packet number changes so the game sees the release.
  struct HoldForGuide {
    PPCContext& ctx;
    uint8_t* base;
    uint32_t state;
    ~HoldForGuide() {
      if (state && ctx.r3.u32 == 0 && GuideIsOpen()) {
        std::memset(base + state + 4, 0, 12);
        REX_STORE_U32(state, REX_LOAD_U32(state) ^ 0x80000000u);
      }
    }
  } hold_for_guide{ctx, base, state};
  // The pause card takes the state the game would have got, keyboard included, while it is open.
  struct HoldForCard {
    PPCContext& ctx;
    uint8_t* base;
    uint32_t user;
    uint32_t state;
    ~HoldForCard() {
      if (state && ctx.r3.u32 == 0) KameoPauseCardTakePad(ctx, base, user, state);
    }
  } hold_for_card{ctx, base, user, state};
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
  // Player two's controller is the first one when player one plays on the keyboard, which the runtime hands to player one; it is read from there.
  if (user == 1 && ctx.r3.u32 != 0 && KameoPlayerUsesKeyboard(0) && CoopInputRouteActive() && !KameoPlayerUsesKeyboard(1)) {
    ctx.r3.u64 = 0;
    ctx.r4.u32 = state;
    __imp__sub_826BA2F0(ctx, base);
  }
  rex::xam::X_INPUT_STATE physical{};
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
  if (user == 0) g_standbyPadButtons.store(KCVAR_GET(kameo_input_mode) != 1 && !KCVAR_GET(kameo_story_splitscreen) ? uint16_t(RemapPad(physical.gamepad).buttons) : 0, std::memory_order_relaxed);
  if (user == 0 && ctx.r3.u32 == 0 && FeedPadCapture(physical.gamepad)) {
    rex::xam::X_INPUT_STATE neutral{};
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
  rex::xam::X_INPUT_STATE keyboard{};
  rex::xam::X_INPUT_STATE aggregate{};
  const bool have_aggregate = ReadAggregateKeyboardState(&aggregate);
  rex::xam::X_INPUT_STATE device_keyboard{};
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
    // Aiming warriors and the aim cameras read the right stick themselves instead of cameraDefaultJoyProcess, so while it is not taking the mouse the travel is handed to the stick.
    if (g_mouseLook && g_padPolls[player] - g_mouseLookPoll[player] > 2) {
      float dx = 0.0f;
      float dy = 0.0f;
      TakePhysicalMouseTravel(int(user), &dx, &dy);
      const float tick = std::clamp(LoadFloat(base, kFrameTime), 1.0f / 240.0f, 0.1f);
      const float scale = kRadiansPerPixel * float(KCVAR_GET(MnkSensitivity())) / (kAimTurnRate * tick);
      if (keyboard.gamepad.thumb_rx == 0) keyboard.gamepad.thumb_rx = MouseStickAxis(dx * scale);
      if (keyboard.gamepad.thumb_ry == 0) keyboard.gamepad.thumb_ry = MouseStickAxis(-dy * scale);
    }
  }
  if (!g_mouseLook) {
    if (user == 0 && !KCVAR_GET(kameo_story_splitscreen)) {
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
      const int32_t notch = kScrollPerDetent;
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
    // The pause card takes the mouse itself, so its clicks are not presses as well.
    if (KameoPauseCardOpen()) left = right = false;
    if (left) buttons |= rex::xam::kXInputGamepadA;
    // "Press Start" has no menu to click, so there the left button is Start as well.
    if (left && AtTitleScreen()) buttons |= rex::xam::kXInputGamepadStart;
    if (right) buttons |= rex::xam::kXInputGamepadB;
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
  SuppressPlayerOneActivation(user, &keyboard.gamepad);
  // The game reads a changed packet number as new input.
  static std::array<std::array<uint8_t, sizeof(keyboard.gamepad)>, 2> last_output{};
  static std::array<uint32_t, 2> packet{};
  if (std::memcmp(&keyboard.gamepad, last_output[player].data(), sizeof(keyboard.gamepad)) != 0) {
    std::memcpy(last_output[player].data(), &keyboard.gamepad, sizeof(keyboard.gamepad));
    ++packet[player];
  }
  keyboard.packet_number = packet[player];
  std::memcpy(base + state, &keyboard, sizeof(keyboard));
  ctx.r3.u64 = 0;
}
