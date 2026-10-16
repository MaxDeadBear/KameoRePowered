// Camera options: the follow camera held at its default distance, and a field of view offset.

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>

#include "engine/guest_addresses.h"

REXCVAR_DEFINE_BOOL(kameo_camera_drift, false, "Kameo", "true restores the original follow camera, which falls further behind and higher the longer you run; false keeps it at its default distance.");
REXCVAR_DEFINE_BOOL(kameo_camera_auto_level, false, "Kameo", "true lets the follow camera tilt itself back to its default angle while you move, as the original game did.");
REXCVAR_DEFINE_INT32(kameo_camera_fov, 0, "Kameo", "Degrees added to the gameplay camera's vertical field of view, -10 to 30; cutscenes keep their own.");

namespace {

// pitch_0 and the 0.95 LazyCamCalculateDefaultPos scales the smoothed default distance by.
constexpr uint32_t kHalfPi = 0x82706F94;
constexpr uint32_t kDefaultDistanceScale = 0x82711768;
// A camera with this flag at +136 is a cutscene camera (cameraIsCutCameraActive).
constexpr uint32_t kCutCameraFlag = 0x2000;
constexpr uint32_t kFrameTime = 0x82755760;
// How quickly the fixed-distance camera closes on its place, per second; it trails a running player by their speed divided by this, and no further.
constexpr float kFollowRate = 10.0f;

}  // namespace

using kameo::engine::LoadGuestFloat;
using kameo::engine::StoreGuestFloat;

// LazyCamCalculateLazyPos(camera, out) -- the follow camera only closes on its distance at a rate scaled by the frame time twice, so it trails further the longer the player runs; unless the drift is wanted, the position it settles on keeps its direction from the look point but is put back at the default distance and pitch, where cameraCalculatePos would place it.
extern "C" REX_FUNC(sub_82440D28) {
  const uint32_t camera = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  // While the player moves with the camera stick at rest, this eases the pitch at camera+200 back to its default, which drags a camera the player tilted up or down back toward it.
  const bool keep_pitch = camera && !REXCVAR_GET(kameo_camera_drift) && !REXCVAR_GET(kameo_camera_auto_level);
  const uint32_t pitch_bits = keep_pitch ? REX_LOAD_U32(camera + 200) : 0;
  __imp__sub_82440D28(ctx, base);
  if (keep_pitch) REX_STORE_U32(camera + 200, pitch_bits);
  if (REXCVAR_GET(kameo_camera_drift) || !camera || !out) return;
  const uint32_t record = REX_LOAD_U32(camera + 252);
  if (!record) return;
  const float distance = LoadGuestFloat(base, record + 92) * LoadGuestFloat(base, kDefaultDistanceScale);
  if (!(distance > 0.0f)) return;
  const float look_x = LoadGuestFloat(base, camera + 172);
  const float look_y = LoadGuestFloat(base, camera + 176);
  const float look_z = LoadGuestFloat(base, camera + 180);
  float dx = LoadGuestFloat(base, out) - look_x;
  float dz = LoadGuestFloat(base, out + 8) - look_z;
  float length = std::sqrt(dx * dx + dz * dz);
  if (length < 1e-4f) {
    const float yaw = LoadGuestFloat(base, camera + 196);
    dx = std::sin(yaw);
    dz = std::cos(yaw);
    length = 1.0f;
  }
  // cameraCalculatePos offsets by sin(pitch + pi/2) horizontally and cos(pitch + pi/2) vertically.
  const float pitch = LoadGuestFloat(base, camera + 200) + LoadGuestFloat(base, kHalfPi);
  const float horizontal = std::abs(std::sin(pitch)) * distance / length;
  const float target[3] = {look_x + dx * horizontal, look_y + std::cos(pitch) * distance, look_z + dz * horizontal};
  // The look point bobs with the walk cycle and leads the player by a per-frame step, so the camera eases toward its place instead of copying that shake; record+124 is last tick's position before geometry avoidance pulled it in.
  const float previous[3] = {LoadGuestFloat(base, record + 124), LoadGuestFloat(base, record + 128), LoadGuestFloat(base, record + 132)};
  const float gap = std::sqrt((target[0] - previous[0]) * (target[0] - previous[0]) + (target[1] - previous[1]) * (target[1] - previous[1]) + (target[2] - previous[2]) * (target[2] - previous[2]));
  const float frame_time = std::clamp(LoadGuestFloat(base, kFrameTime), 0.0f, 0.1f);
  const float follow = gap > distance ? 1.0f : 1.0f - std::exp(-kFollowRate * frame_time);
  for (uint32_t i = 0; i < 3; ++i) StoreGuestFloat(base, out + i * 4, previous[i] + (target[i] - previous[i]) * follow);
}

// cameraCalculateView(player, matrix) -- returns the vertical field of view mainSetupMainRenderInfo renders and culls with; the option widens it for gameplay cameras only, leaving the camera's own state and its distance, which follows its field of view, alone.
extern "C" REX_FUNC(sub_82275620) {
  const uint32_t player = ctx.r3.u32;
  __imp__sub_82275620(ctx, base);
  const int32_t offset = std::clamp(REXCVAR_GET(kameo_camera_fov), -10, 30);
  if (offset == 0) return;
  PPCContext active = ctx;
  active.r3.u64 = player;
  sub_822777F8(active, base);
  const uint32_t camera = active.r3.u32;
  if (!camera || (REX_LOAD_U32(camera + 136) & kCutCameraFlag) != 0) return;
  const double fov = ctx.f1.f64;
  if (fov > 1.0 && fov < 170.0) ctx.f1.f64 = std::clamp(fov + offset, 10.0, 150.0);
}

