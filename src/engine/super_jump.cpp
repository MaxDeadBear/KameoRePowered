// Super jump: holding LB floats the player up (LT to sink) and steers with the left stick, then lets them glide down on low gravity.

#include <cmath>
#include <cstring>

#include "engine/guest_addresses.h"
#include "engine/super_jump.h"

std::atomic<uint32_t> g_kameo_super_jump_enabled{1};

namespace kameo::engine {
namespace {

// An XINPUT_GAMEPAD in guest memory: wButtons, bLeftTrigger, sThumbLX, sThumbLY.
constexpr uint32_t kPadButtons = 0x82B35514;
constexpr uint32_t kPadLeftTrigger = 0x82B35516;
constexpr uint32_t kPadLeftStickX = 0x82B35518;
constexpr uint32_t kPadLeftStickY = 0x82B3551A;

void ApplyFlightSteering(uint8_t* base, uint32_t actor, float flight_speed, float sx, float sy) {
  uint32_t px_bits = REX_LOAD_U32(actor + 512);
  uint32_t pz_bits = REX_LOAD_U32(actor + 520);
  float px = 0.0f, pz = 0.0f;
  std::memcpy(&px, &px_bits, sizeof(px));
  std::memcpy(&pz, &pz_bits, sizeof(pz));

  float cam_yaw = 0.0f;
  bool have_cam = false;
  const uint32_t pComp = REX_LOAD_U32(actor + 2672);
  if (pComp != 0) {
    uint32_t cx_bits = REX_LOAD_U32(pComp + 1060);
    uint32_t cz_bits = REX_LOAD_U32(pComp + 1068);
    float cx = 0.0f, cz = 0.0f;
    std::memcpy(&cx, &cx_bits, sizeof(cx));
    std::memcpy(&cz, &cz_bits, sizeof(cz));
    float cdx = cx - px;
    float cdz = cz - pz;
    if ((cdx * cdx + cdz * cdz) > 0.01f) {
      cam_yaw = std::atan2(cdx, cdz);
      have_cam = true;
    }
  }
  if (!have_cam) {
    uint32_t yaw_bits = REX_LOAD_U32(actor + 568);
    std::memcpy(&cam_yaw, &yaw_bits, sizeof(cam_yaw));
  }

  const float fx = std::sin(cam_yaw);
  const float fz = std::cos(cam_yaw);
  const float rx = std::cos(cam_yaw);
  const float rz = -std::sin(cam_yaw);

  const float dx = (fx * sy - rx * sx) * flight_speed;
  const float dz = (fz * sy - rz * sx) * flight_speed;

  px += dx;
  pz += dz;
  std::memcpy(&px_bits, &px, sizeof(px_bits));
  std::memcpy(&pz_bits, &pz, sizeof(pz_bits));
  REX_STORE_U32(actor + 512, px_bits);
  REX_STORE_U32(actor + 520, pz_bits);

  float face_yaw = std::atan2(dx, dz);
  uint32_t yaw_bits;
  std::memcpy(&yaw_bits, &face_yaw, sizeof(yaw_bits));
  REX_STORE_U32(actor + 568, yaw_bits);
  REX_STORE_U32(actor + 712, yaw_bits);
}

}  // namespace
}  // namespace kameo::engine

using namespace kameo::engine;

// Called from the per-frame game tick.
void KameoUpdateSuperJumpFloat(PPCContext& ctx, uint8_t* base) {
  if (g_kameo_super_jump_enabled.load(std::memory_order_acquire) == 0 || !base) {
    return;
  }

  const uint16_t buttons = REX_LOAD_U16(kPadButtons);
  bool lb_held = (buttons & 0x0100) != 0;

  const uint8_t lt_val = REX_LOAD_U8(kPadLeftTrigger);
  bool descend = (lt_val > 30);

  const int16_t raw_sx = static_cast<int16_t>(REX_LOAD_U16(kPadLeftStickX));
  const int16_t raw_sy = static_cast<int16_t>(REX_LOAD_U16(kPadLeftStickY));

  float sx = 0.0f;
  float sy = 0.0f;
  if (std::abs(raw_sx) > 4000) {
    sx = static_cast<float>(raw_sx) / 32768.0f;
  }
  if (std::abs(raw_sy) > 4000) {
    sy = static_cast<float>(raw_sy) / 32768.0f;
  }

  // Keyboard and mouse reach the pad state above through their bindings (LB, left trigger, left stick), so no keys are read here directly; Space is jump alone.
  float stick_len = std::sqrt(sx * sx + sy * sy);
  if (stick_len > 1.0f) {
    sx /= stick_len;
    sy /= stick_len;
    stick_len = 1.0f;
  }

  const uint32_t player_root = REX_LOAD_U32(kPlayerList);
  if (player_root == 0) {
    return;
  }
  const uint32_t actor = REX_LOAD_U32(player_root + 0x10);
  if (actor == 0) {
    return;
  }

  static bool s_was_floating = false;
  static bool s_slow_falling = false;

  if (lb_held) {
    s_was_floating = true;
    s_slow_falling = false;

    uint32_t y_bits = REX_LOAD_U32(actor + 516);
    float y;
    std::memcpy(&y, &y_bits, sizeof(y));
    if (descend) {
      y -= 0.30f;
    } else {
      y += 0.25f;
    }
    std::memcpy(&y_bits, &y, sizeof(y_bits));
    REX_STORE_U32(actor + 516, y_bits);

    REX_STORE_U32(actor + 692, 0);
    REX_STORE_U32(actor + 704, 0);

    uint32_t flags = REX_LOAD_U32(actor + 2440);
    flags &= ~1u;
    REX_STORE_U32(actor + 2440, flags);

    if (stick_len > 0.1f) {
      ApplyFlightSteering(base, actor, 0.50f * stick_len, sx, sy);
    }
  } else if (s_was_floating) {
    s_was_floating = false;
    s_slow_falling = true;
  }

  if (!lb_held && s_slow_falling) {
    const uint32_t flags = REX_LOAD_U32(actor + 2440);
    uint32_t dist_bits = REX_LOAD_U32(actor + 688);
    float dist = 0.0f;
    std::memcpy(&dist, &dist_bits, sizeof(dist));

    if ((flags & 1u) != 0 || (dist <= 0.25f && dist >= 0.0f)) {
      s_slow_falling = false;
      float one = 1.0f;
      uint32_t one_bits;
      std::memcpy(&one_bits, &one, sizeof(one_bits));
      REX_STORE_U32(actor + 704, one_bits);
    } else {
      uint32_t vy_bits = REX_LOAD_U32(actor + 692);
      float vy = 0.0f;
      std::memcpy(&vy, &vy_bits, sizeof(vy));
      if (vy < -2.0f) {
        vy = -2.0f;
        std::memcpy(&vy_bits, &vy, sizeof(vy_bits));
        REX_STORE_U32(actor + 692, vy_bits);
      }
      float low_grav = 0.10f;
      uint32_t low_bits;
      std::memcpy(&low_bits, &low_grav, sizeof(low_bits));
      REX_STORE_U32(actor + 704, low_bits);

      if (stick_len > 0.1f) {
        ApplyFlightSteering(base, actor, 0.35f * stick_len, sx, sy);
      }
    }
  }
}
