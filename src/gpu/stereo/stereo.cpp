#include "gpu/stereo/stereo.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include <rex/logging.h>

#include "gpu/draw/draw_constants.h"
#include "gpu/draw/draw_types.h"

REXCVAR_DEFINE_INT32(kameo_gfx_stereo, 0, "Kameo", "Stereoscopic 3D: 0 off, 1 side by side (each eye in its half of the picture, for 3D TVs and side-by-side headsets), 2 top and bottom, 3 red/cyan anaglyph for any screen. The scene is drawn once per eye, so the GPU does about twice the work.");
REXCVAR_DEFINE_INT32(kameo_gfx_stereo_depth, 25, "Kameo", "How far apart the two eyes' views are, in tenths of a percent of the picture's width at infinity: 25 puts the horizon 2.5% of the screen apart between the eyes. More depth is a stronger effect and harder to fuse; 10 - 50 is the usual range.");
REXCVAR_DEFINE_INT32(kameo_gfx_stereo_focus, 100, "Kameo", "Which depth sits exactly on the screen, in percent of the camera's distance from Kameo: 100 puts her on the screen, less brings the world out of it, more pushes it behind.");
REXCVAR_DEFINE_BOOL(kameo_gfx_stereo_swap, false, "Kameo", "Swap the left and right eyes, for a display that expects the right eye first.");

namespace kameo::gpu {

namespace {

// Written by the game thread's camera hook, read by the render thread; before the first gameplay camera, a depth that suits the menu's storybook.
std::atomic<float> g_focusDistance{8.0f};

}  // namespace

StereoMode ActiveStereoMode() {
  const int32_t mode = REXCVAR_GET(kameo_gfx_stereo);
  if (mode < 1 || mode > 3) return StereoMode::Off;
  return StereoMode(mode);
}

float StereoSeparation() {
  return float(std::clamp(REXCVAR_GET(kameo_gfx_stereo_depth), 0, 200)) / 1000.0f;
}

void NoteStereoFocusDistance(float distance) {
  if (!std::isfinite(distance) || distance <= 0.01f || distance >= 100000.0f) return;
  const float previous = g_focusDistance.exchange(distance, std::memory_order_relaxed);
  // Logged when it moves by a quarter, so a report shows the depths the 3D was converging on.
  static float logged = 0.0f;
  if (ActiveStereoMode() != StereoMode::Off && (logged == 0.0f || std::abs(distance - logged) > logged * 0.25f)) {
    logged = distance;
    REXLOG_INFO("[kameo-gfx] stereo focus distance {:.2f} (was {:.2f})", distance, previous);
  }
}

bool StereoShiftsDraw(const DrawCall& call, const DrawFrame& frame, bool into_scene) {
  // The fixed-function shaders draw the HUD, but also the light glows (xfix_pliteLightGlow_drawWithOcclusionQuery), which are world-space billboards with a full perspective matrix in c0-c3; only a matrix without perspective is flat 2D.
  if (call.hud_2d) {
    if (call.vs_constants == kNoData || call.vs_constant_bytes < 64) return false;
    const float* c = reinterpret_cast<const float*>(frame.arena.data() + call.vs_constants);
    if (std::abs(c[12]) <= 1e-4f && std::abs(c[13]) <= 1e-4f && std::abs(c[14]) <= 1e-4f) return false;
  }
  if (into_scene) return true;
  if (call.depth_only) return false;
  const float vw = call.viewport[2] > 0 ? call.viewport[2] : float(detail::kGuestWidth);
  const float vh = call.viewport[3] > 0 ? call.viewport[3] : float(detail::kGuestHeight);
  return std::abs(vw * float(detail::kGuestHeight) - vh * float(detail::kGuestWidth)) <= 0.02f * vh * float(detail::kGuestWidth);
}

void StereoEyeShift(uint32_t eye, float out[2]) {
  // The shift is in clip units, where the picture is 2 wide, so a fraction f of the width is 2f; split between the two eyes, each moves by f.
  const float separation = StereoSeparation();
  const bool left = (eye == 0) != REXCVAR_GET(kameo_gfx_stereo_swap);
  out[0] = left ? -separation : separation;
  const float focus = float(std::clamp(REXCVAR_GET(kameo_gfx_stereo_focus), 10, 1000)) / 100.0f;
  out[1] = g_focusDistance.load(std::memory_order_relaxed) * focus;
}

}  // namespace kameo::gpu
