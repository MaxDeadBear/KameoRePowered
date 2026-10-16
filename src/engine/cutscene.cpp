// Skippable cutscenes: Start runs an in-engine scene to its end, and every Bink movie takes the game's own skip.

#include <algorithm>

#include <rex/cvar.h>

#include "core/time.h"
#include "engine/cutscene.h"
#include "engine/guest_addresses.h"

REXCVAR_DEFINE_BOOL(kameo_skip_cutscenes, true, "Kameo", "true lets Start skip in-engine cutscenes and every FMV (A also skips FMVs, as the game already allowed for some).");

using namespace kameo::engine;

namespace {

constexpr uint32_t kButtonStart = 0x10;
// A movie with the camera (movie +116 == 2, set by mxmlCameraCue) is a cutscene; +269 marks one that loops back from its end marker.
constexpr uint32_t kMovieCameraMode = 116;
constexpr uint8_t kMovieHasCamera = 2;
constexpr uint32_t kMovieTime = 136;
constexpr uint32_t kMovieEventCount = 92;
constexpr uint32_t kMovieEvents = 96;
constexpr uint32_t kMovieLoops = 269;
constexpr uint32_t kFmvSkippable = 212;

// Start pressed this tick on any playing controller, read the way fmvTick reads its skip buttons.
bool StartPressed(uint8_t* base) {
  int32_t players = int32_t(REX_LOAD_U32(kNumPlayers));
  if (players <= 0) players = 4;
  const uint32_t sticks = REX_LOAD_U32(kSticks);
  const uint32_t mask = REX_LOAD_U32(kCommitTestButtonMask);
  for (int32_t i = 0; i < std::min(players, 4); ++i) {
    const uint32_t controller = REX_LOAD_U8(kPlayerControllers + 36 * uint32_t(i));
    if ((REX_LOAD_U32(sticks + controller * 320 + 8) & mask & kButtonStart) != 0) return true;
  }
  return false;
}

}  // namespace

// _movieUpdate(movie, dt) -- advances the scene clock at +136 and runs every event whose millisecond time at +4 it passes, in order, until one (mxmlEndMovie sets the end state 6 and returns 0) stops it; Start stretches this step to the last event, so the scene's flags, fades, actor placement and end callback all still happen and only the waiting is skipped.
extern "C" REX_FUNC(sub_822D85F8) {
  const uint32_t movie = ctx.r3.u32;
  if (movie && REXCVAR_GET(kameo_skip_cutscenes) && REX_LOAD_U8(movie + kMovieCameraMode) == kMovieHasCamera && REX_LOAD_U8(movie + kMovieLoops) == 0 && StartPressed(base)) {
    const uint32_t events = REX_LOAD_U32(movie + kMovieEvents);
    uint32_t last_ms = 0;
    for (uint32_t i = 0, count = events ? REX_LOAD_U32(movie + kMovieEventCount) : 0; i < count; ++i) {
      const uint32_t event = REX_LOAD_U32(events + 4 * i);
      if (event) last_ms = std::max(last_ms, REX_LOAD_U32(event + 4));
    }
    const double remaining = last_ms / 1000.0 - double(LoadGuestFloat(base, movie + kMovieTime)) + 0.05;
    if (remaining > ctx.f1.f64) {
      REXLOG_INFO("[kameo] cutscene skipped {:.1f}s ahead", remaining);
      ctx.f1.f64 = remaining;
    }
  }
  __imp__sub_822D85F8(ctx, base);
}

int64_t g_kameo_credits_requested_ms = 0;

// fmvloaded(fmv, request) -- copies the play request into the movie, its first word landing at +212, which fmvTick requires before A or Start ends the movie.
extern "C" REX_FUNC(sub_82264B80) {
  constexpr uint32_t kFmvName = 192;
  constexpr int64_t kCreditsRequestMs = 2000;
  const uint32_t fmv = ctx.r3.u32;
  // Only the ending's level defines a "Credits" object, so the options start the always-defined "Death" movie and point it at Credits.bik before its player opens the file.
  if (fmv && g_kameo_credits_requested_ms && KameoSteadyMilliseconds() - g_kameo_credits_requested_ms < kCreditsRequestMs) {
    g_kameo_credits_requested_ms = 0;
    const char death[] = "Death";
    bool is_death = true;
    for (uint32_t i = 0; i < sizeof(death); ++i) is_death = is_death && REX_LOAD_U8(fmv + kFmvName + i) == uint8_t(death[i]);
    if (is_death) {
      WriteGuestStringAt(base, fmv + kFmvName, "Credits");
      REXLOG_INFO("[kameo] credits: movie object {:08X} renamed to Credits", fmv);
    } else {
      REXLOG_WARN("[kameo] credits: loaded movie object {:08X} is not the Death movie", fmv);
    }
  }
  __imp__sub_82264B80(ctx, base);
  if (fmv && REXCVAR_GET(kameo_skip_cutscenes)) {
    if (const uint32_t record = REX_LOAD_U32(fmv + 16)) REX_STORE_U32(record + kFmvSkippable, 1);
  }
}

// fmvPlay(fmv, mode, skippable, ...) -- sets the same flag for a movie started again.
extern "C" REX_FUNC(sub_82264AA8) {
  const uint32_t fmv = ctx.r3.u32;
  __imp__sub_82264AA8(ctx, base);
  if (fmv && REXCVAR_GET(kameo_skip_cutscenes)) {
    if (const uint32_t record = REX_LOAD_U32(fmv + 16)) REX_STORE_U32(record + kFmvSkippable, 1);
  }
}

