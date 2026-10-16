// What the Resume, Credits and Level Select lines ask for, carried out where the game closes a gameplay pause itself.

#include "core/time.h"
#include "engine/cutscene.h"
#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;

namespace {

// fmvLoadAndPlay(name, skippable, fade time, _, _, _, _, _, _; stack +84 ending, +92 fade, +100 done callback, +108 its argument) with the arguments _EndOfGameFmvEnded plays the credits with, less its callback into the ending and its fade out, so play carries on where it was.
void PlayCredits(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kDeathName = 0x820755F8;
  constexpr uint32_t kFmvFadeTime = 0x8273BB34;
  constexpr uint32_t kFrameBytes = 256;
  PPCContext call = ctx;
  call.r1.u64 = ctx.r1.u32 - kFrameBytes;
  REX_STORE_U32(call.r1.u32, ctx.r1.u32);
  call.r3.u64 = kDeathName;
  call.r4.u64 = 1;
  call.r5.u64 = 0;
  call.r6.u64 = 1;
  call.r7.u64 = 1;
  call.r8.u64 = 1;
  call.r9.u64 = 1;
  call.r10.u64 = 0;
  call.f1.f64 = LoadFloat(base, kFmvFadeTime);
  for (uint32_t offset : {84u, 92u, 100u, 108u}) REX_STORE_U32(call.r1.u32 + offset, 0);
  g_kameo_credits_requested_ms = KameoSteadyMilliseconds();
  sub_82264C08(call, base);
  REXLOG_INFO("[kameo] credits started from the options");
}

}  // namespace

// xfixgem_storybook_initSimplePauseMenuWidget() -- where Start closes a gameplay pause (storedPauseValue bit 0x10): clearing the bit from both pause words, resetting objectPauseFrames, killing the pause objects and playing the close sound; the Resume line asks for the same.
extern "C" REX_FUNC(sub_82269838) {
  __imp__sub_82269838(ctx, base);
  if (!g_resumeRequested) return;
  g_resumeRequested = false;
  constexpr uint32_t kStoredPauseValue = 0x82B71904;
  constexpr uint32_t kPauseRequestFlags = 0x82B71900;
  constexpr uint32_t kObjectPauseFrames = 0x82B71908;
  constexpr uint32_t kNextPauseValue = 0x82884890;
  constexpr uint32_t kSimplePauseObject = 0x82884884;
  constexpr uint32_t kPauseCloseSound = 0xD639B25;
  const uint32_t pause_value = REX_LOAD_U32(kStoredPauseValue);
  if (REX_LOAD_U32(kSimplePauseObject) && (pause_value & 0x10) != 0) {
    REX_STORE_U32(kPauseRequestFlags, REX_LOAD_U32(kPauseRequestFlags) & ~0x10u);
    REX_STORE_U32(kObjectPauseFrames, 0);
    CallGuest(ctx, base, sub_82269560, 0);
    CallGuest(ctx, base, sub_822EA728, kPauseCloseSound);
    REX_STORE_U32(kNextPauseValue, pause_value & ~0x10u);
  }
  if (g_playCreditsRequested) {
    g_playCreditsRequested = false;
    PlayCredits(ctx, base);
  }
  StartPendingLevelJump(ctx, base);
}
