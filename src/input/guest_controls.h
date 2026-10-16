// Guest globals the game's controller and menu code read, shared by the input hooks and the menus that read buttons themselves, and float access to guest memory.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/guest.h"

namespace kameo::input {

// player_list: the first player's object, null on the title screen, in the menus and between levels; player two's follows at +4.
constexpr uint32_t kPlayerList = 0x8280D1A0;
// Seconds the last game tick took, as a float.
constexpr uint32_t kFrameTime = 0x82755760;
// One record per player, 36 bytes apart, whose first byte is the controller that player reads.
constexpr uint32_t kPlayerControllers = 0x82732F48;
constexpr uint32_t kPlayerControllerBytes = 36;
// Points at the per-controller stick records, 320 bytes each.
constexpr uint32_t kSticks = 0x8276706C;
constexpr uint32_t kStickBytes = 320;
constexpr uint32_t kStickHeld = 0;
constexpr uint32_t kStickPressed = 8;
constexpr uint32_t kStickLeftX = 292;
constexpr uint32_t kStickLeftY = 296;
constexpr uint32_t kCommitTestButtonMask = 0x82731F54;
// Per-player control settings, 148 bytes each: +0 inverts the camera's horizontal turn and +4 its vertical one.
constexpr uint32_t kPlayerControlSettings = 0x828855B0;
constexpr uint32_t kPlayerControlSettingsBytes = 148;
// Stick-record buttons use the XInput layout.
constexpr uint32_t kButtonA = 0x1000;
constexpr uint32_t kButtonB = 0x2000;
constexpr uint32_t kButtonX = 0x4000;
constexpr uint32_t kButtonY = 0x8000;

inline float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  REX_STORE_U32(address, bits);
}

inline uint32_t ControllerStick(const uint8_t* base, uint32_t controller) { return REX_LOAD_U32(kSticks) + controller * kStickBytes; }

// The stick record of the controller `player` (clamped to 0-3) reads.
inline uint32_t PlayerStick(const uint8_t* base, int8_t player) { return ControllerStick(base, REX_LOAD_U8(kPlayerControllers + kPlayerControllerBytes * uint32_t(std::clamp<int8_t>(player, 0, 3)))); }

}  // namespace kameo::input
