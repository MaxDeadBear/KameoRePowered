// Player-to-player teleport (gather, goto, rejoin) and player 2's triple-tap that toggles split-screen.

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "engine/splitscreen_internal.h"
#include "input/input.h"

REXCVAR_DECLARE(int32_t, kameo_p2_input_mode);

REXCVAR_DEFINE_DOUBLE(kameo_splitscreen_rejoin_offset, 2.0, "Kameo",
                      "Sideways offset used when player 2 rejoins or is "
                      "teleported to player 1 (world units).");

// D-pad Up rather than Start: the button is read but not consumed, and three Starts left the pause menu open with no way for player 2 to dismiss it.
REXCVAR_DEFINE_BOOL(kameo_p2_triple_tap, true, "Kameo",
                    "Player 2 can triple-tap D-pad Up to join or leave split-screen.");
REXCVAR_DEFINE_UINT32(kameo_p2_triple_tap_button, 0x0001, "Kameo",
                      "Button mask player 2 triple-taps (default 0x0001 = D-pad Up).");
REXCVAR_DEFINE_INT32(kameo_p2_triple_tap_frames, 90, "Kameo",
                     "Frames allowed for player 2's three taps.");

// Momentary triggers for a player stuck on geometry: the next frame performs the warp and clears them.
REXCVAR_DEFINE_BOOL(kameo_gather_players, false, "Kameo",
                    "Teleport every other player to player 1, then self-clear.");
REXCVAR_DEFINE_BOOL(kameo_goto_player2, false, "Kameo",
                    "Teleport player 1 to player 2, then self-clear.");

namespace kameo::engine {
namespace {

// objectAskObject message ids, read out of triggerWarpPlayersBehindTrigger and objectWarpAllPlayers.
constexpr uint32_t kMsgGetWarpableObject = 0x689298A;  // player -> warpable obj
constexpr uint32_t kMsgGetPosition = 0x1235863;        // obj -> float[3] out
constexpr uint32_t kMsgCameraPostWarp = 0xFA23B6E;     // camera settle

// Guest heap objects live around 0x4xxxxxxx and statics at 0x82xxxxxx; anything below 16MB is a transient non-pointer.
bool PlausibleGuestPtr(uint32_t v) {
  return v >= 0x01000000u && v < 0xC0000000u;
}

// The guest is big-endian but REX_LOAD_U32/REX_STORE_U32 already swap, so a plain bit-cast is right.
void AddGuestFloat(uint8_t* base, uint32_t addr, float delta) {
  (void)base;
  uint32_t bits = REX_LOAD_U32(addr);
  float v;
  std::memcpy(&v, &bits, sizeof(v));
  v += delta;
  std::memcpy(&bits, &v, sizeof(bits));
  REX_STORE_U32(addr, bits);
}

// The warrior wheel's read: buttons = *(*Sticks + pad * 320) & commitTestButtonMask; raw_pad reads a physical pad slot, which stays live while player 2 is benched.
uint32_t ReadPadButtons(uint8_t* base, uint32_t player_no, bool raw_pad = false) {
  (void)base;
  const uint32_t pad =
      raw_pad ? player_no : REX_LOAD_U8(kPlayerControllers + 36 * player_no);
  const uint32_t rol = (pad << 2) | (pad >> 30);
  const uint32_t sticks_base = REX_LOAD_U32(kSticks);
  if (!PlausibleGuestPtr(sticks_base)) {
    return 0;
  }
  return REX_LOAD_U32(sticks_base + ((pad + rol) << 6)) &
         REX_LOAD_U32(kCommitTestButtonMask);
}

}  // namespace

uint32_t AskObject(PPCContext& src, uint8_t* base, uint32_t obj, uint32_t msg, uint32_t arg) {
  PPCContext c = src;
  c.r3.u64 = obj;
  c.r4.u64 = 0;
  c.r5.u64 = msg;
  c.r6.u64 = arg;
  ObjectAskObject(c, base);
  return c.r3.u32;
}

// Kameo spends most of the game morphed, so the player resolves through the object currently active for him, as triggerWarpPlayersBehindTrigger does.
uint32_t ActiveObjectFor(PPCContext& src, uint8_t* base, uint32_t player) {
  if (player == 0) {
    return 0;
  }
  PPCContext c = src;
  c.r3.u64 = player;
  ObjectGetActiveObj(c, base);
  return c.r3.u32;
}

// kMsgGetWarpableObject comes back null while morphed, so it is only the fallback behind objectGetActiveObj.
uint32_t WarpableObjectFor(PPCContext& src, uint8_t* base, uint32_t player, uint32_t scratch_out) {
  if (player == 0) {
    return 0;
  }
  const uint32_t active = ActiveObjectFor(src, base, player);
  REX_STORE_U32(scratch_out, 0);
  AskObject(src, base, player, kMsgGetWarpableObject, scratch_out);
  const uint32_t warpable = REX_LOAD_U32(scratch_out);
  const uint32_t chosen = active ? active : (warpable ? warpable : player);
  return chosen;
}

// Moves player dst_idx onto player src_idx's position, then re-blends the moved player's camera so it does not lerp across the level.
bool WarpPlayerToPlayer(PPCContext& ctx, uint8_t* base, uint32_t dst_idx, uint32_t src_idx) {
  const uint32_t dst_player = REX_LOAD_U32(kPlayerList + dst_idx * 4);
  const uint32_t src_player = REX_LOAD_U32(kPlayerList + src_idx * 4);
  if (dst_player == 0 || src_player == 0) {
    return false;
  }

  const uint32_t frame = ScratchFrame(ctx.r1.u32);
  const uint32_t scratch_ptr = frame + 0x40;    // one guest pointer
  const uint32_t scratch_pos = frame + 0x50;    // float[4] position
  const uint32_t scratch_blend = frame + 0x70;  // camera blend descriptor

  PPCContext call = ctx;
  call.r1.u64 = frame;

  const uint32_t dst_obj = WarpableObjectFor(call, base, dst_player, scratch_ptr);
  const uint32_t src_obj = WarpableObjectFor(call, base, src_player, scratch_ptr);
  if (dst_obj == 0 || src_obj == 0) {
    REXLOG_WARN("[splitscreen]   abort: dst_obj={:08X} src_obj={:08X}", dst_obj, src_obj);
    return false;
  }

  for (uint32_t i = 0; i < 4; ++i) {
    REX_STORE_U32(scratch_pos + i * 4, 0);
  }

  // Warrior forms answer kMsgGetPosition with -1, so fall back to the vtable getter as triggerWarpPlayersBehindTrigger does: (*(*obj + 136))(obj[1], &pos).
  {
    const int32_t rc = static_cast<int32_t>(
        AskObject(call, base, src_obj, kMsgGetPosition, scratch_pos));
    if (rc == -1) {
      const uint32_t vtable = REX_LOAD_U32(src_obj);
      const uint32_t getter = vtable ? REX_LOAD_U32(vtable + 136) : 0;
      if (getter != 0) {
        PPCContext g = call;
        g.r3.u64 = REX_LOAD_U32(src_obj + 4);  // obj[1]
        g.r4.u64 = scratch_pos;
        g.ctr.u64 = getter;
        // REX_CALL_INDIRECT_FUNC expands against whatever `ctx` is in scope, so the prepared context shadows the parameter.
        {
          PPCContext& ctx = g;
          REX_CALL_INDIRECT_FUNC(getter);
        }
      }
    }
  }

  // Sideways on X so the arriving player does not land inside the target.
  const float offset =
      static_cast<float>(REXCVAR_GET(kameo_splitscreen_rejoin_offset));
  if (offset != 0.0f) {
    AddGuestFloat(base, scratch_pos + 0, offset);
  }

  // A null angle keeps the player facing wherever they already were.
  PPCContext w = call;
  w.r3.u64 = dst_obj;
  w.r4.u64 = scratch_pos;
  w.r5.u64 = 0;
  ObjectWarpObjectPosAngle(w, base);

  PPCContext cam = call;
  cam.r3.u64 = dst_idx;
  CameraGetActiveCamera(cam, base);
  const uint32_t active_cam = cam.r3.u32;
  if (active_cam != 0) {
    PPCContext b = call;
    b.r3.u64 = dst_idx;
    b.r4.u64 = scratch_blend;
    CameraSetupDefaultBlend(b, base);
    REX_STORE_U32(scratch_blend, 1);  // blend mode used by the trigger warp

    PPCContext s = call;
    s.r3.u64 = dst_idx;
    s.r4.u64 = scratch_blend;
    s.r5.u64 = active_cam;
    CameraSetBlend(s, base);

    AskObject(call, base, active_cam, kMsgCameraPostWarp, 0);
  }
  return true;
}

// Three taps inside the window toggle kameo_story_splitscreen, which KameoSplitScreenTick then acts on.
void ServicePlayer2TripleTap(uint8_t* base) {
  if (!REXCVAR_GET(kameo_p2_triple_tap)) {
    return;
  }
  const bool first_pad_standby = !REXCVAR_GET(kameo_story_splitscreen) && KameoPlayerUsesKeyboard(0);
  const uint32_t buttons = first_pad_standby ? KameoStandbyPadButtons() & REX_LOAD_U32(kCommitTestButtonMask) : ReadPadButtons(base, 1, /*raw_pad=*/true);
  const uint32_t tap_mask = REXCVAR_GET(kameo_p2_triple_tap_button);
  const bool start_down = tap_mask != 0 && (buttons & tap_mask) != 0;

  static bool s_prev_down = false;
  static uint32_t s_taps = 0;
  static uint32_t s_frames = 0;

  if (s_taps != 0 && ++s_frames > static_cast<uint32_t>(
                                      REXCVAR_GET(kameo_p2_triple_tap_frames))) {
    s_taps = 0;  // window expired
    s_frames = 0;
  }

  if (start_down && !s_prev_down) {
    if (s_taps == 0) {
      s_frames = 0;
    }
    if (++s_taps >= 3) {
      const bool now = !REXCVAR_GET(kameo_story_splitscreen);
      if (now) REXCVAR_SET(kameo_p2_input_mode, 0);
      REXCVAR_SET(kameo_story_splitscreen, now);
      REXLOG_INFO("[splitscreen] player 2 triple-tap (mask {:04X}) -> split-screen {}",
                  tap_mask, now ? "ON" : "OFF");
      s_taps = 0;
      s_frames = 0;
    }
  }
  s_prev_down = start_down;
}

void ServiceTeleportRequests(uint8_t* base) {
  const bool gather = REXCVAR_GET(kameo_gather_players);
  const bool goto_p2 = REXCVAR_GET(kameo_goto_player2);
  if (!gather && !goto_p2) {
    return;
  }

  PPCContext* ctx = CurrentGuestContext();
  if (!ctx) {
    return;
  }

  if (gather) {
    REXCVAR_SET(kameo_gather_players, false);
    for (uint32_t i = 1; i < 4; ++i) {
      if (WarpPlayerToPlayer(*ctx, base, i, 0)) {
        REXLOG_INFO("[splitscreen] warped player {} to player 1", i + 1);
      }
    }
  }
  if (goto_p2) {
    REXCVAR_SET(kameo_goto_player2, false);
    if (WarpPlayerToPlayer(*ctx, base, 0, 1)) {
      REXLOG_INFO("[splitscreen] warped player 1 to player 2");
    }
  }
}

}  // namespace kameo::engine
