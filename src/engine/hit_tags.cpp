// Hit tags at any frame rate.
//
// When an attack lands, _AddHitRecord tags the target's hits record (+180 the tag count, then 8 bytes a tag from +184: the attacker's hit sphere, a
// landed-this-frame flag at +185 and a time to live at +186) so the same attack can't hit it again until the tag runs out. The time to live comes from the hit
// sphere in 60 Hz frames, and hitsTagTick takes (int)(frameHertz + 0.5), at least 1, off it every frame. Above 60 fps that is still 1 a frame, so tags run out
// sooner (2.75 times at 165 fps) and held or lingering attacks hit again sooner, from the player and the enemies alike.
// Here each record counts the 60 Hz frames that have passed and the tick takes off only the whole ones, so tags last as long as at 60 fps; frames with
// none to take only clear the landed-this-frame flags, as the tick does every frame.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <rex/cvar.h>

#include "engine/guest_addresses.h"

REXCVAR_DEFINE_BOOL(kameo_fix_hit_tags, true, "Kameo", "Let an attack's hit tag (which stops it hitting the same target twice) run out by time rather than per frame, so attacks don't hit more often at high frame rates. Off restores the per-frame count, which runs tags out faster the higher the frame rate.");

namespace {

using kameo::engine::LoadGuestFloat;
using kameo::engine::StoreGuestFloat;

// frameTime * 60, the frames at 60 Hz this frame stands for.
constexpr uint32_t kFrameHertz = 0x8275A07C;
constexpr uint32_t kTagCount = 180;
constexpr uint32_t kFirstTagLanded = 185;
constexpr uint32_t kTagSize = 8;

// The 60 Hz frames each record has not yet taken off; only the game thread ticks hits.
std::unordered_map<uint32_t, float> g_pending;

}  // namespace

// hitsTagTick(hits)
extern "C" REX_FUNC(sub_822DC8B0) {
  const uint32_t hits = ctx.r3.u32;
  if (!hits || !REXCVAR_GET(kameo_fix_hit_tags)) {
    __imp__sub_822DC8B0(ctx, base);
    return;
  }
  const int count = int8_t(REX_LOAD_U8(hits + kTagCount));
  if (count <= 0) {
    g_pending.erase(hits);
    return;
  }
  const float frame_hertz = LoadGuestFloat(base, kFrameHertz);
  float& pending = g_pending[hits];
  pending += std::max(frame_hertz, 0.0f);
  const float whole = std::floor(pending);
  if (whole < 1.0f) {
    for (int i = 0; i < count; ++i) REX_STORE_U8(hits + kFirstTagLanded + kTagSize * uint32_t(i), 0);
    return;
  }
  pending -= whole;
  // The tick rounds frameHertz + 0.5, so a whole number goes through as it is.
  StoreGuestFloat(base, kFrameHertz, whole);
  __imp__sub_822DC8B0(ctx, base);
  StoreGuestFloat(base, kFrameHertz, frame_hertz);
}
