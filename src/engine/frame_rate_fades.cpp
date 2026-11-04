// Fades that stall at high frame rates.
//
// Some characters fade by a 16-bit opacity (0 gone, 190 fully there) moved by (int)(frame time * rate) every frame. Each step is cut to a whole number, so it
// shrinks with the frame rate and reaches 0 once the frame rate passes the rate: the fade stops and whatever waits for it waits for ever.
//   The shadow troll (shadowTroll_EX.c), opacity at +78 of its record (*(object + 2680) + 364), mirrored into the object at +2472:
//     appear, summon and smash-ground fade it in at 150 until it reaches 120 (an animation does the rest); above 150 fps it stays a swarm of bats and the fight never goes on.
//     run-away fades it in at 100, disappear fades it out at 200.
//     die fades it at 50 over the end of its death animation, then at 200 once it has played; it is only finished below 2, so above 200 fps it never dies.
//   The swamp ogre (_ProActSwampOgreDie) fades its body out at 120 at the end of its death and is only removed at 0.
// After each of those states runs, the step it took is redone by time at the console's rate (it ticked at 30 Hz, so it took the same whole step 30 times a
// second), the fraction cut off carried to the next frame, and whatever the game does at the end of the fade is done when this one gets there.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <rex/cvar.h>

#include "engine/guest_addresses.h"

REXCVAR_DEFINE_BOOL(kameo_fix_frame_rate_fades, true, "Kameo", "Fade the shadow troll and the swamp ogre's body by time rather than by whole steps a frame, as on the console. Off restores the per-frame steps, which stop moving at high frame rates: above 150 fps the shadow troll never re-forms from its bats.");

namespace {

using kameo::engine::LoadGuestFloat;

// The measured frame time (frameTime), capped by kamVideoFrameTimer at 0.1 s.
constexpr uint32_t kFrameTime = 0x82755760;
constexpr uint32_t kObjectInstance = 8;        // its +112 is the object's own frame time, read instead of frameTime when present
constexpr uint32_t kInstanceFrameTime = 112;
constexpr uint32_t kObjectModel = 12;          // its +316 is the animation the death states watch
constexpr uint32_t kModelAnimation = 316;
constexpr uint32_t kStreamProgress = 228;      // 0 to 1 through the animation animationGetEndOfStream returns
constexpr uint32_t kObjectPhase = 384;         // the current state's step
constexpr uint32_t kObjectOpacity = 2472;
constexpr uint32_t kObjectTrollState = 2680;   // the troll's state: +12 its AI step, +364 its record, +544 its flags
constexpr uint32_t kTrollAiStep = 12;
constexpr uint32_t kTrollStateRecord = 364;
constexpr uint32_t kTrollStateFlags = 544;
constexpr uint32_t kTrollFinished = 0x20000;   // set once the death fade is through
constexpr uint32_t kRecordOpacity = 78;
constexpr uint32_t kRecordFlags = 96;
constexpr uint32_t kRecordHoldFlag = 8;        // the disappear state may hold the troll visible instead of fading it (a tutorial moment)

constexpr int kAnimatedFrom = 120;             // the troll's fade-ins hand over to an animation at this opacity
constexpr int kOpaque = 190;
constexpr float kConsoleTicksPerSecond = 30.0f;

// Each fade's whole step per console tick: its rate (the constant times frame time) at 1/30 s, cut as the console cut it.
constexpr int kTrollFadeInStep = 5;            // flt_8273A7E0, 150
constexpr int kTrollFadeOutStep = 6;           // accelMult_0, 200
constexpr int kTrollRunawayStep = 3;           // 100
constexpr int kTrollDyingStep = 1;             // flt_8273BB14, 50
constexpr int kOgreDeathStep = 4;              // flt_827118A8, 120

// Where in their death animations the troll and the ogre start fading.
constexpr float kTrollDyingFrom = 0.83725488f;
constexpr float kOgreDyingFrom = 0.95f;        // flt_82711768

// The fraction each object's last step dropped; only the game thread runs these states.
std::unordered_map<uint32_t, float> g_carry;

float ObjectFrameTime(uint8_t* base, uint32_t object) {
  const uint32_t instance = REX_LOAD_U32(object + kObjectInstance);
  return instance ? LoadGuestFloat(base, instance + kInstanceFrameTime) : LoadGuestFloat(base, kFrameTime);
}

// animationGetEndOfStream(animation, 0)'s progress for the object, as the death states read it; -1 without an animation.
float AnimationProgress(PPCContext& ctx, uint8_t* base, uint32_t object) {
  const uint32_t model = REX_LOAD_U32(object + kObjectModel);
  if (!model) return -1.0f;
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(model + kModelAnimation);
  call.r4.u64 = 0;
  __imp__sub_82291D20(call, base);
  return call.r3.u32 ? LoadGuestFloat(base, call.r3.u32 + kStreamProgress) : -1.0f;
}

// The whole step this frame for a fade of `per_tick` a console tick, carrying the fraction for `object`.
int StepThisFrame(uint32_t object, float frame_time, int per_tick) {
  float& carry = g_carry[object];
  const float exact = float(per_tick) * kConsoleTicksPerSecond * std::clamp(frame_time, 0.0f, 0.1f) + carry;
  const float whole = std::floor(exact);
  carry = exact - whole;
  return int(whole);
}

struct Troll {
  uint32_t object = 0;
  uint32_t state = 0;
  uint32_t record = 0;
  uint8_t phase = 0;
  int opacity = 0;
  float frame_time = 0.0f;
};

Troll ReadTroll(uint8_t* base, uint32_t object) {
  Troll t;
  if (!object) return t;
  t.state = REX_LOAD_U32(object + kObjectTrollState);
  if (!t.state) return t;
  t.record = REX_LOAD_U32(t.state + kTrollStateRecord);
  if (!t.record) return t;
  t.object = object;
  t.phase = REX_LOAD_U8(object + kObjectPhase);
  t.opacity = int16_t(REX_LOAD_U16(t.record + kRecordOpacity));
  t.frame_time = ObjectFrameTime(base, object);
  return t;
}

bool Fixing(const Troll& t) { return t.record && REXCVAR_GET(kameo_fix_frame_rate_fades); }

// Redoes the step the state just took from `t.opacity`, by `per_tick` a console tick (negative fades out); returns the opacity written.
int StepTroll(uint8_t* base, const Troll& t, int per_tick) {
  const int step = StepThisFrame(t.object, t.frame_time, std::abs(per_tick));
  const int opacity = std::clamp(t.opacity + (per_tick < 0 ? -step : step), 0, kOpaque);
  REX_STORE_U16(t.record + kRecordOpacity, uint16_t(opacity));
  REX_STORE_U16(t.object + kObjectOpacity, uint16_t(opacity));
  return opacity;
}

// The troll's fade-ins: while `fading` and the opacity is under 120, the state adds (int)(frame time * rate).
void AfterFadeIn(uint8_t* base, const Troll& before, bool fading, int per_tick) {
  if (!Fixing(before)) return;
  if (!fading || before.opacity >= kAnimatedFrom) {
    g_carry.erase(before.object);
    return;
  }
  StepTroll(base, before, per_tick);
}

}  // namespace

// _shadowTroll_ProActAppear: step 0 starts, step 1 waits on a timer, steps 2 and on fade in.
extern "C" REX_FUNC(sub_821E3960) {
  const Troll before = ReadTroll(base, ctx.r3.u32);
  __imp__sub_821E3960(ctx, base);
  AfterFadeIn(base, before, before.phase >= 2, kTrollFadeInStep);
}

// _shadowTroll_ProActSummon: step 1 fades in.
extern "C" REX_FUNC(sub_821E6D48) {
  const Troll before = ReadTroll(base, ctx.r3.u32);
  __imp__sub_821E6D48(ctx, base);
  AfterFadeIn(base, before, before.phase == 1, kTrollFadeInStep);
}

// xfix_shadowTroll_ProActSmashGround: step 1 fades in.
extern "C" REX_FUNC(sub_821E5E60) {
  const Troll before = ReadTroll(base, ctx.r3.u32);
  __imp__sub_821E5E60(ctx, base);
  AfterFadeIn(base, before, before.phase == 1, kTrollFadeInStep);
}

// _shadowTroll_AIRunaway: its AI step 2 fades the troll in.
extern "C" REX_FUNC(sub_821EA2C0) {
  const Troll before = ReadTroll(base, ctx.r3.u32);
  const uint8_t ai_step = before.state ? REX_LOAD_U8(before.state + kTrollAiStep) : 0;
  __imp__sub_821EA2C0(ctx, base);
  AfterFadeIn(base, before, ai_step == 2, kTrollRunawayStep);
}

// xfix_shadowTroll_ProActDisappear: fades out every frame, unless the hold flag keeps it visible. With that flag set the fade or the hold is decided by a
// task and an event, so those frames are left as the game ran them: they only come up in a tutorial moment.
extern "C" REX_FUNC(sub_821E36C0) {
  const Troll before = ReadTroll(base, ctx.r3.u32);
  __imp__sub_821E36C0(ctx, base);
  if (!Fixing(before)) return;
  if (REX_LOAD_U32(before.record + kRecordFlags) & kRecordHoldFlag || before.opacity <= 0) {
    g_carry.erase(before.object);
    return;
  }
  StepTroll(base, before, -kTrollFadeOutStep);
}

// _shadowTroll_ProActDie: past 0.837 of the death animation it fades out by 50, and once the animation has played by 200 more; below 2 the troll is finished.
extern "C" REX_FUNC(sub_821E5970) {
  const Troll before = ReadTroll(base, ctx.r3.u32);
  const float progress = before.record && before.phase ? AnimationProgress(ctx, base, before.object) : -1.0f;
  __imp__sub_821E5970(ctx, base);
  if (!Fixing(before)) return;
  if (!before.phase || progress <= kTrollDyingFrom || before.opacity <= 0) {
    g_carry.erase(before.object);
    return;
  }
  const bool played = progress == 1.0f;
  const int opacity = StepTroll(base, before, -(kTrollDyingStep + (played ? kTrollFadeOutStep : 0)));
  if (played && opacity <= 1) REX_STORE_U32(before.state + kTrollStateFlags, REX_LOAD_U32(before.state + kTrollStateFlags) | kTrollFinished);
}

// _ProActSwampOgreDie: in step 1, past 0.95 of the death animation, the body fades out by 120 and is removed at 0.
extern "C" REX_FUNC(sub_82511C58) {
  const uint32_t object = ctx.r3.u32;
  const bool fixing = object && REXCVAR_GET(kameo_fix_frame_rate_fades) && REX_LOAD_U8(object + kObjectPhase) == 1;
  const float progress = fixing ? AnimationProgress(ctx, base, object) : -1.0f;
  const int before = fixing ? int16_t(REX_LOAD_U16(object + kObjectOpacity)) : 0;
  const float frame_time = fixing ? ObjectFrameTime(base, object) : 0.0f;
  __imp__sub_82511C58(ctx, base);
  if (!fixing) return;
  if (progress <= kOgreDyingFrom || before <= 0) {
    g_carry.erase(object);
    return;
  }
  // At 0 the game has removed it already.
  if (int16_t(REX_LOAD_U16(object + kObjectOpacity)) == 0) {
    g_carry.erase(object);
    return;
  }
  const int opacity = std::max(before - StepThisFrame(object, frame_time, kOgreDeathStep), 0);
  REX_STORE_U16(object + kObjectOpacity, uint16_t(opacity));
  if (opacity == 0) {
    g_carry.erase(object);
    PPCContext call = ctx;
    call.r3.u64 = object;
    kameo::engine::ObjectKill(call, base);
  }
}
