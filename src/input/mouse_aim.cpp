// Mouse look and mouse aiming: the camera, Chilla's and Deep Blue's aim, and the warrior wheel, each fed the mouse's travel directly.

#include <algorithm>
#include <array>
#include <cmath>

#include <rex/cvar.h>

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"

REXCVAR_DECLARE(double, mnk_sensitivity);

using namespace kameo::input;
using namespace kameo::input::detail;

namespace {

constexpr uint32_t kCameraDisableJoy = 0x20;
// Deep Blue's spray pitch follows the right stick's height times -0.5 radians (WaterCannonTick).
constexpr float kSprayPitchRange = 0.5f;
constexpr float kTwoPi = 6.2831855f;

// When handleWarriorWheelInput last ran for each keyboard player, and the mouse travel gathered into that wheel's direction since it opened.
std::array<int64_t, 2> g_wheelSeen{};
std::array<float, 2> g_wheelX{};
std::array<float, 2> g_wheelY{};
float g_sprayPitchStick = 0.0f;
uint32_t g_sprayObject = 0;

// Keeps a yaw within one turn either side of zero.
float WrapYaw(float yaw) {
  while (yaw > kTwoPi) yaw -= kTwoPi;
  while (yaw < -kTwoPi) yaw += kTwoPi;
  return yaw;
}

// Takes the mouse travel for a warrior that aims by it directly, when player one's controls record is the one ticking; the stick then leaves the mouse out.
bool TakeAimTravel(uint8_t* base, uint32_t controls, float* dx, float* dy) {
  if (!controls || !g_mouseCaptured || !g_mouseLook) return false;
  const int player = int(REX_LOAD_U8(controls + 14));
  if (player < 0 || player > 1 || !KameoPlayerUsesKeyboard(player) || WarriorWheelOpen(player)) return false;
  g_mouseLookPoll[size_t(player)] = g_padPolls[size_t(player)];
  TakePhysicalMouseTravel(player, dx, dy);
  return true;
}

}  // namespace

bool kameo::input::detail::WarriorWheelOpen(int player) { return KameoSteadyMilliseconds() - g_wheelSeen[size_t(player)] < 150; }

// cameraDefaultJoyProcess(camera, maxYawSpeed, maxPitchSpeed, maxPitch, minPitch, accel, decel) -- the stick only sets a capped turning speed, so mouse look adds the mouse's travel straight onto the yaw at camera+196 and pitch at camera+200, within the same pitch limits.
extern "C" REX_FUNC(sub_82276638) {
  const uint32_t camera = ctx.r3.u32;
  const float max_pitch = float(ctx.f3.f64);
  const float min_pitch = float(ctx.f4.f64);
  __imp__sub_82276638(ctx, base);
  if (!camera || !g_mouseLook) return;
  if ((REX_LOAD_U32(camera + 136) & kCameraDisableJoy) != 0) return;
  const uint32_t player = REX_LOAD_U32(camera + 116);
  if (player > 1 || !KameoPlayerUsesKeyboard(int(player)) || WarriorWheelOpen(int(player))) return;
  g_mouseLookPoll[size_t(player)] = g_padPolls[size_t(player)];
  float dx = 0.0f;
  float dy = 0.0f;
  TakePhysicalMouseTravel(int(player), &dx, &dy);
  if (dx == 0.0f && dy == 0.0f) return;
  const float scale = kRadiansPerPixel * float(REXCVAR_GET(mnk_sensitivity));
  if (REX_LOAD_U32(kPlayerControlSettings + kPlayerControlSettingsBytes * player) != 0) dx = -dx;
  if (REX_LOAD_U32(kPlayerControlSettings + 4 + kPlayerControlSettingsBytes * player) != 0) dy = -dy;
  StoreFloat(base, camera + 196, WrapYaw(LoadFloat(base, camera + 196) - dx * scale));
  // LazyCamera only blends toward the stick-driven yaw while the turn speed at camera+208 is non-zero, and otherwise recomputes the yaw from its follow position; a negligible speed keeps that blend engaged.
  if (dx != 0.0f && LoadFloat(base, camera + 208) == 0.0f) StoreFloat(base, camera + 208, dx > 0.0f ? 0.0001f : -0.0001f);
  const float pitch = std::clamp(LoadFloat(base, camera + 200) + dy * scale, std::min(min_pitch, max_pitch), std::max(min_pitch, max_pitch));
  StoreFloat(base, camera + 200, pitch);
}

// xfix_objCameraFollowTick_8222BF68(yeti) -- Chilla's aim step (called from throwIceRightState and xfix_iceYeti_throwIceState): with the aim mode at +565 of the 2680 record set it turns the yaw at +568 by the stick, and in mode 2 the pitch at +440 of that record, clamped to 60 degrees and copied to +564; mouse travel is added to both first, as mouse look does for the camera.
extern "C" REX_FUNC(sub_8222BF68) {
  const uint32_t yeti = ctx.r3.u32;
  const uint32_t record = yeti ? REX_LOAD_U32(yeti + 2680) : 0;
  const uint8_t mode = record ? REX_LOAD_U8(record + 565) : 0;
  float dx = 0.0f;
  float dy = 0.0f;
  if (mode != 0 && TakeAimTravel(base, REX_LOAD_U32(yeti + 2672), &dx, &dy)) {
    const float scale = kRadiansPerPixel * float(REXCVAR_GET(mnk_sensitivity));
    const uint32_t keyboard_player = REX_LOAD_U8(REX_LOAD_U32(yeti + 2672) + 14);
    if (REX_LOAD_U32(kPlayerControlSettings + kPlayerControlSettingsBytes * keyboard_player) != 0) dx = -dx;
    if (REX_LOAD_U32(kPlayerControlSettings + 4 + kPlayerControlSettingsBytes * keyboard_player) != 0) dy = -dy;
    StoreFloat(base, yeti + 568, WrapYaw(LoadFloat(base, yeti + 568) - dx * scale));
    if (mode == 2) StoreFloat(base, record + 440, LoadFloat(base, record + 440) - dy * scale);
  }
  __imp__sub_8222BF68(ctx, base);
}

// PlrKeysReady(player) -- copies the sticks into the player's controls record; while Deep Blue sprays, the mouse turns its yaw directly and moves a held spray height in place of the right stick.
extern "C" REX_FUNC(sub_8217BEC8) {
  const uint32_t player = ctx.r3.u32;
  __imp__sub_8217BEC8(ctx, base);
  TraceP2Controls(base, player);
  if (!player || player != g_sprayObject) return;
  const uint32_t controls = REX_LOAD_U32(player + 2672);
  float dx = 0.0f;
  float dy = 0.0f;
  if (!TakeAimTravel(base, controls, &dx, &dy)) return;
  if (dx != 0.0f) {
    if (REX_LOAD_U32(kPlayerControlSettings + kPlayerControlSettingsBytes * REX_LOAD_U8(controls + 14)) != 0) dx = -dx;
    float yaw = LoadFloat(base, player + 568) - dx * kRadiansPerPixel * float(REXCVAR_GET(mnk_sensitivity));
    while (yaw >= kTwoPi) yaw -= kTwoPi;
    while (yaw < 0.0f) yaw += kTwoPi;
    StoreFloat(base, player + 568, yaw);
  }
  const float stick = LoadFloat(base, controls + 88);
  // A controller or the camera keys still move the spray as the game means them to.
  if (stick != 0.0f) {
    g_sprayPitchStick = stick;
    return;
  }
  const float scale = kRadiansPerPixel * float(REXCVAR_GET(mnk_sensitivity)) / kSprayPitchRange;
  g_sprayPitchStick = std::clamp(g_sprayPitchStick - dy * scale, -1.0f, 1.0f);
  StoreFloat(base, controls + 88, g_sprayPitchStick);
}

// WaterCannonTick(deepBlue) -- sets +1452 of the 2680 record while Deep Blue's spray aims; the Deep Blue doing so is kept for PlrKeysReady.
extern "C" REX_FUNC(sub_82503F98) {
  const uint32_t deep_blue = ctx.r3.u32;
  __imp__sub_82503F98(ctx, base);
  const uint32_t record = deep_blue ? REX_LOAD_U32(deep_blue + 2680) : 0;
  if (record && REX_LOAD_U32(record + 1452) != 0) {
    g_sprayObject = deep_blue;
  } else if (g_sprayObject == deep_blue) {
    g_sprayObject = 0;
    g_sprayPitchStick = 0.0f;
  }
}

// handleWarriorWheelInput(wheel) -- picks the warrior whose slot lies nearest the left stick's angle, from Sticks+292/+296 of the wheel player's controller; the movement keys only give eight angles for up to ten warriors, so mouse travel gathered into a held direction stands in for the stick.
extern "C" REX_FUNC(sub_823C2FF8) {
  const uint32_t wheel = ctx.r3.u32;
  const int8_t player = wheel ? int8_t(REX_LOAD_U8(REX_LOAD_U32(wheel + 8) + 146)) : -1;
  if (player < 0 || player > 1 || !KameoPlayerUsesKeyboard(player)) {
    __imp__sub_823C2FF8(ctx, base);
    return;
  }
  const int64_t now = KameoSteadyMilliseconds();
  if (now - g_wheelSeen[size_t(player)] > 250) {
    g_wheelX[size_t(player)] = 0.0f;
    g_wheelY[size_t(player)] = 0.0f;
  }
  g_wheelSeen[size_t(player)] = now;
  g_mouseLookPoll[size_t(player)] = g_padPolls[size_t(player)];
  float dx = 0.0f;
  float dy = 0.0f;
  TakePhysicalMouseTravel(player, &dx, &dy);
  g_wheelX[size_t(player)] += dx;
  g_wheelY[size_t(player)] -= dy;
  constexpr float kWheelRadius = 100.0f;
  constexpr float kWheelDeadZone = 30.0f;
  const float length = std::sqrt(g_wheelX[size_t(player)] * g_wheelX[size_t(player)] + g_wheelY[size_t(player)] * g_wheelY[size_t(player)]);
  if (length > kWheelRadius) {
    g_wheelX[size_t(player)] *= kWheelRadius / length;
    g_wheelY[size_t(player)] *= kWheelRadius / length;
  }
  const uint32_t stick = PlayerStick(base, player);
  const float key_x = LoadFloat(base, stick + kStickLeftX);
  const float key_y = LoadFloat(base, stick + kStickLeftY);
  if (std::abs(key_x) > 0.1f || std::abs(key_y) > 0.1f) {
    g_wheelX[size_t(player)] = 0.0f;
    g_wheelY[size_t(player)] = 0.0f;
  }
  if (length < kWheelDeadZone || key_x != 0.0f || key_y != 0.0f) {
    __imp__sub_823C2FF8(ctx, base);
    return;
  }
  StoreFloat(base, stick + kStickLeftX, g_wheelX[size_t(player)] / std::min(length, kWheelRadius));
  StoreFloat(base, stick + kStickLeftY, g_wheelY[size_t(player)] / std::min(length, kWheelRadius));
  __imp__sub_823C2FF8(ctx, base);
  StoreFloat(base, stick + kStickLeftX, key_x);
  StoreFloat(base, stick + kStickLeftY, key_y);
}
