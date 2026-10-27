#pragma once

// Stereoscopic 3D: the scene is replayed once per eye with a clip-space shift, x += s * (w - c), which every recompiled vertex shader applies from the shared constants. That is an off-axis stereo pair: an eye moved sideways by s * c / focal, converging at depth c, so anything at the convergence depth sits on the screen and the rest stands in front of or behind it.
// The feature lives in this folder; every line elsewhere that touches it is tagged `STEREO:`. See README.md here for how to remove it.

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, kameo_gfx_stereo);
REXCVAR_DECLARE(int32_t, kameo_gfx_stereo_depth);
REXCVAR_DECLARE(int32_t, kameo_gfx_stereo_focus);
REXCVAR_DECLARE(bool, kameo_gfx_stereo_swap);

namespace kameo::gpu {

struct DrawCall;
struct DrawFrame;

enum class StereoMode : int32_t {
  Off = 0,
  SideBySide = 1,  // each eye squeezed into its half of the picture, the format 3D TVs and side-by-side headsets take
  TopBottom = 2,
  Anaglyph = 3,  // red/cyan glasses on any screen
};

StereoMode ActiveStereoMode();
// One eye without stereo, two with.
inline uint32_t StereoEyeCount() { return ActiveStereoMode() == StereoMode::Off ? 1u : 2u; }
// The shared-constant shift for an eye: x is the signed separation in clip units (negative for the left eye, so distant things sit to its left), y the convergence depth in world units.
void StereoEyeShift(uint32_t eye, float out[2]);
// The separation as a fraction of the picture's width, which is how far the guest's culling frustum has to widen so the shift pulls nothing in that it culled.
float StereoSeparation();
// The follow camera's distance from the player, in world units, noted each time the game places it; the convergence depth follows it, so Kameo stays on the screen plane. Cutscenes and menus keep the last one.
void NoteStereoFocusDistance(float distance);
// Whether a draw is seen through the main camera and so takes the eye's shift: anything drawn into the scene, and offscreen passes of the scene's 16:9 shape (the glow and water passes draw at a reduced size and composite back over the screen). Shadow maps (depth only), the square imposter atlas and the flat 2D HUD stay where the guest put them.
bool StereoShiftsDraw(const DrawCall& call, const DrawFrame& frame, bool into_scene);

}  // namespace kameo::gpu
