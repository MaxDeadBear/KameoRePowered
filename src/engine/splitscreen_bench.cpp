// Benching player 2 when split-screen is turned off: parked out of sight and switched off through the engine's own passes, never freed.

#include <rex/logging/macros.h>

#include "engine/splitscreen_internal.h"

namespace kameo::engine {
namespace {

struct SwitchedOff {
  uint32_t object = 0;
  uint32_t process = 0;
  uint32_t render = 0;
  uint32_t group_flag = 0;
};

struct BenchedPlayer {
  uint32_t player = 0;
  uint32_t body = 0;
  // Frames left before he is switched off; the warp out of sight has to be drawn once first, because a switched-off object keeps its last drawn frame.
  uint32_t countdown = 0;
  SwitchedOff controller;
  SwitchedOff body_state;
};
BenchedPlayer g_benched;

// Object fields objectProcessPassCheck reads: [16] and [17] at 4 switch processing and rendering off, and flag 0x200 keeps the level's group masks from switching them back on.
// An object handle is {class, object}; the engine's object functions all take the object at +4.
constexpr uint32_t kHandleObject = 4;
constexpr uint32_t kObjProcessPass = 56;
constexpr uint32_t kObjRenderPass = 60;
constexpr uint32_t kObjProcessRequest = 64;
constexpr uint32_t kObjRenderRequest = 68;
constexpr uint32_t kObjFlags = 72;
constexpr uint32_t kObjFlagKilled = 0x1000;
constexpr uint32_t kObjFlagNoGroup = 0x200;
constexpr uint32_t kPassDisabled = 4;

// -100000.0f, well under any level.
constexpr uint32_t kFarBelow = 0xC7C35000u;

SwitchedOff SwitchOff(uint8_t* base, uint32_t game_object) {
  const uint32_t object = game_object ? REX_LOAD_U32(game_object + kHandleObject) : 0;
  if (!object) return {};
  const uint32_t flags = REX_LOAD_U32(object + kObjFlags);
  const SwitchedOff saved{object, REX_LOAD_U32(object + kObjProcessRequest), REX_LOAD_U32(object + kObjRenderRequest), flags & kObjFlagNoGroup};
  REX_STORE_U32(object + kObjFlags, flags | kObjFlagNoGroup);
  REX_STORE_U32(object + kObjProcessRequest, kPassDisabled);
  REX_STORE_U32(object + kObjRenderRequest, kPassDisabled);
  // Moved now rather than on the object's next pass check, which an object outside the ticked arrays may never reach.
  if (PPCContext* ctx = CurrentGuestContext()) {
    PPCContext a = *ctx;
    a.r1.u64 = ScratchFrame(ctx->r1.u32);
    a.r3.u64 = object;
    a.r4.u64 = kPassDisabled;
    if (REX_LOAD_U32(object + kObjProcessPass) != kPassDisabled) ObjectSetProcessPass(a, base);
    a.r3.u64 = object;
    a.r4.u64 = kPassDisabled;
    if (REX_LOAD_U32(object + kObjRenderPass) != kPassDisabled) ObjectSetRenderPass(a, base);
  }
  // The same call objectClassKill makes, so its looping sounds do not carry on without it.
  if (PPCContext* ctx = CurrentGuestContext()) {
    PPCContext a = *ctx;
    a.r1.u64 = ScratchFrame(ctx->r1.u32);
    a.r3.u64 = object;
    AudioStopObj(a, base);
  }
  return saved;
}

bool SwitchedOffAlive(uint8_t* base, const SwitchedOff& saved) {
  (void)base;
  return !saved.object || (REX_LOAD_U32(saved.object + kObjFlags) & kObjFlagKilled) == 0;
}

void SwitchOn(uint8_t* base, const SwitchedOff& saved) {
  (void)base;
  if (!saved.object) return;
  REX_STORE_U32(saved.object + kObjProcessRequest, saved.process);
  REX_STORE_U32(saved.object + kObjRenderRequest, saved.render);
  REX_STORE_U32(saved.object + kObjFlags, (REX_LOAD_U32(saved.object + kObjFlags) & ~kObjFlagNoGroup) | saved.group_flag);
}

// Switches a benched player back on without re-adding him, so a level teardown can delete him: objectKillAll waits for every object to finish dying, and a switched-off one never does.
void ReleaseBench(uint8_t* base) {
  SwitchOn(base, g_benched.controller);
  SwitchOn(base, g_benched.body_state);
  g_benched = {};
}

}  // namespace

// Destroying player 2 frees XACT memory he co-owns and corrupts the heap, so he is moved far below the level instead.
bool ParkPlayerObject(uint8_t* base, uint32_t player2) {
  if (player2 == 0) {
    return false;
  }
  PPCContext* ctx = CurrentGuestContext();
  if (!ctx) {
    return false;
  }

  const uint32_t frame = ScratchFrame(ctx->r1.u32);
  const uint32_t scratch_ptr = frame + 0x40;
  const uint32_t scratch_pos = frame + 0x50;

  PPCContext call = *ctx;
  call.r1.u64 = frame;

  const uint32_t obj = WarpableObjectFor(call, base, player2, scratch_ptr);
  if (obj == 0) {
    return false;
  }

  REX_STORE_U32(scratch_pos + 0, 0);
  REX_STORE_U32(scratch_pos + 4, kFarBelow);
  REX_STORE_U32(scratch_pos + 8, 0);
  REX_STORE_U32(scratch_pos + 12, 0);

  PPCContext w = call;
  w.r3.u64 = obj;
  w.r4.u64 = scratch_pos;
  w.r5.u64 = 0;
  ObjectWarpObjectPosAngle(w, base);
  g_collapse_view.store(1, std::memory_order_release);
  REXLOG_INFO("[splitscreen] parked player object {:08X} out of play", player2);
  return true;
}

bool BenchPlayer2(uint8_t* base) {
  const uint32_t player2 = REX_LOAD_U32(kPlayerList + 4);
  PPCContext* ctx = CurrentGuestContext();
  if (!player2 || !ctx) return false;
  // player_list holds the player; the body the camera sees is the active object it reports, which can be a separate object.
  PPCContext call = *ctx;
  call.r1.u64 = ScratchFrame(ctx->r1.u32);
  const uint32_t body = WarpableObjectFor(call, base, player2, call.r1.u32 + 0x40);
  REXLOG_INFO("[splitscreen] bench: player 2 {:08X} object {:08X}, active {:08X}, body {:08X} object {:08X}", player2, REX_LOAD_U32(player2 + kHandleObject), ActiveObjectFor(call, base, player2), body, body ? REX_LOAD_U32(body + kHandleObject) : 0);
  ParkPlayerObject(base, player2);
  g_benched = {player2, body, 3};
  // numWantedPlayers stays at 2 so his pad is still polled and he can tap back in.
  REX_STORE_U32(kPlayerList + 4, 0);
  if (REX_LOAD_U32(kNumPlayers) > 1) REX_STORE_U32(kNumPlayers, 1);
  g_collapse_view.store(1, std::memory_order_release);
  REXLOG_INFO("[splitscreen] benched player 2 {:08X} (body {:08X})", player2, body);
  return true;
}

// Switches the benched player off once his out-of-sight position has been drawn.
void TickBench(uint8_t* base) {
  if (!g_benched.player || !g_benched.countdown || --g_benched.countdown) return;
  g_benched.controller = SwitchOff(base, g_benched.player);
  g_benched.body_state = g_benched.body && g_benched.body != g_benched.player ? SwitchOff(base, g_benched.body) : SwitchedOff{};
  const uint32_t object = g_benched.controller.object;
  if (object) REXLOG_INFO("[splitscreen] bench: object {:08X} passes process {} render {} requests {} {} flags {:08X}", object, REX_LOAD_U32(object + kObjProcessPass), REX_LOAD_U32(object + kObjRenderPass), REX_LOAD_U32(object + kObjProcessRequest), REX_LOAD_U32(object + kObjRenderRequest), REX_LOAD_U32(object + kObjFlags));
}

// Puts a benched player 2 back into player_list and play, beside player 1.
bool UnbenchPlayer2(PPCContext& ctx, uint8_t* base) {
  const BenchedPlayer benched = g_benched;
  if (!benched.player) return false;
  if ((benched.controller.object && REX_LOAD_U32(benched.player + kHandleObject) != benched.controller.object) || !SwitchedOffAlive(base, benched.controller) || !SwitchedOffAlive(base, benched.body_state)) {
    g_benched = {};
    REXLOG_WARN("[splitscreen] benched player 2 {:08X} is gone; spawning a new one", benched.player);
    return false;
  }
  SwitchOn(base, benched.controller);
  SwitchOn(base, benched.body_state);
  REX_STORE_U32(kPlayerList + 4, benched.player);
  if (REX_LOAD_U32(kNumPlayers) < 2) REX_STORE_U32(kNumPlayers, 2);
  REX_STORE_U32(kIsSplitScreenMode, 1);
  if (REX_LOAD_U32(kNumWantedPlayers) < 2) REX_STORE_U32(kNumWantedPlayers, 2);
  g_benched = {};
  WarpPlayerToPlayer(ctx, base, 1, 0);
  g_collapse_view.store(0, std::memory_order_release);
  REXLOG_INFO("[splitscreen] player 2 {:08X} back in beside player 1", benched.player);
  return true;
}

bool Player2Benched() { return g_benched.player != 0; }

void ForgetBenchedPlayer() { g_benched = {}; }

}  // namespace kameo::engine

using namespace kameo::engine;

// gameobjRenderBufferObject(object) -- queues an object's model for the render thread; a benched player's is never queued.
extern "C" REX_FUNC(sub_8210DED0) {
  const uint32_t object = ctx.r3.u32;
  if (object && g_benched.player && (object == g_benched.controller.object || object == g_benched.body_state.object || object == REX_LOAD_U32(g_benched.player + kHandleObject))) {
    static uint32_t skipped = 0;
    if (skipped++ == 0) REXLOG_INFO("[splitscreen] bench: renderer still queued benched object {:08X}; skipped", object);
    return;
  }
  __imp__sub_8210DED0(ctx, base);
}

// objectKillAll() -- the level teardown; a benched player is switched back on first so he dies with the rest.
extern "C" REX_FUNC(sub_82251068) {
  if (g_benched.player) {
    REXLOG_INFO("[splitscreen] level teardown: releasing benched player 2 {:08X}", g_benched.player);
    ReleaseBench(base);
  }
  __imp__sub_82251068(ctx, base);
}
