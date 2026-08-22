
// kameorepowered - ReXGlue Recompiled Project
//
// Split-screen in the story campaign, plus a player-to-player teleport.
//
// mainSetupMainRenderInfo recomputes the viewport count every frame:
//
//   lwz   r10, numPlayers(r31)        ; or numWantedPlayers
//   cmpwi cr6, r10, 1
//   bgt   cr6, +8
//   li    r10, 1                      ; clamp to one viewport
//   lwz   r11, isSplitScreenMode(r31) ; <-- hook lands right after this
//   stw   r10, var_1FC(r1)            ; viewport count committed
//   cmpwi cr6, r11, 0
//   bne   cr6, <split path>
//
// isSplitScreenMode has to be set for real: cameraObjectTick reads it too, and
// without it the second camera is never created -- forcing only the local
// viewport count produced a half-initialised view and crashed the render thread
// ("Call to invalid or unregistered function at guest address 0x00008000").
//
// But it is also *save state*. saveGameFileOpenedForWriting emits the campaign
// progress block only when
//
//   if (!isSplitScreenMode && !gIsDoingCoopReplay) { ... write progress ... }
//
// and storybookCurrentGameDisplayCorrupt tests the same flag. Holding the global
// at 1 therefore made every save write that block blank, wiping progress.
//
// So: set the global (the camera system needs it) and neutralise it exactly at
// the two read sites that decide save/profile behaviour, by forcing the loaded
// register to 0 there. Those gates only engage while our cvar is on, so a real
// co-op session still saves the way the game intends.

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "kameorepowered_hooks_internal.h"

REXCVAR_DEFINE_BOOL(kameo_story_splitscreen, false, "Kameo",
                    "Force split-screen on in the story campaign.");

// How far to one side player 2 is placed when he rejoins or is gathered, so he
// does not materialise inside player 1. World units; tune to taste.
REXCVAR_DEFINE_DOUBLE(kameo_splitscreen_rejoin_offset, 2.0, "Kameo",
                      "Sideways offset used when player 2 rejoins or is "
                      "teleported to player 1 (world units).");

// Player 2's object type. PlayerReady's own chain spawns "Kai" -- the game's
// co-op second character, who has no warrior wheel because warriors belong to
// Kameo. Setting this true spawns player 2 as whatever player 1 is
// (nextLevelPlayerObject), which should carry warrior access with it.
REXCVAR_DEFINE_BOOL(kameo_player2_as_kameo, false, "Kameo",
                    "Spawn player 2 as player 1's character type instead of Kai "
                    "(gives access to the warrior wheel).");

// Player 2 morphing. Kameo's warrior wheel is a single object that only samples
// its own owner's pad, so player 2 is never polled for morph input. This reads
// player 2's buttons directly and morphs him into whatever player 1 has mapped
// to that slot -- shared assignments, separate buttons.
// Player 2's warrior wheel already works -- he can open it and scroll, and the
// game sends the morph message correctly. MorphValid refuses it because his
// warrior "sack" is empty (every slot reads FFFF). Copying player 1's slot
// assignments across is all that is needed; no input handling is involved.
REXCVAR_DEFINE_BOOL(kameo_player2_morph, true, "Kameo",
                    "Give player 2 the same warriors player 1 has mapped.");

// kaiSackPopulate adds each warrior only if its story event is done:
//   if (eventDone(0x2B)) kaiSackPopulateSack(a1, "FlowerBoxer"); ...
// This adds them all unconditionally, so every warrior is available before the
// shadow trolls hand them over. The game's own conditional adds then no-op.
REXCVAR_DEFINE_BOOL(kameo_unlock_all_warriors, false, "Kameo",
                    "Give every elemental warrior immediately.");

// Player 2 triple-taps D-pad Up to opt in/out of split-screen, so no config edit
// is needed mid-game. Deliberately NOT Start: we only read the button, we do not
// consume it, so triple-tapping Start also opened the pause menu three times and
// left it up with no way for player 2 to dismiss it. D-pad Up is unbound during
// normal play. Masks follow XInput (UP=0x01, DOWN=0x02, LEFT=0x04, RIGHT=0x08,
// START=0x10, BACK=0x20).
REXCVAR_DEFINE_BOOL(kameo_p2_triple_tap, true, "Kameo",
                    "Player 2 can triple-tap D-pad Up to join or leave split-screen.");
REXCVAR_DEFINE_UINT32(kameo_p2_triple_tap_button, 0x0001, "Kameo",
                      "Button mask player 2 triple-taps (default 0x0001 = D-pad Up).");
REXCVAR_DEFINE_INT32(kameo_p2_triple_tap_frames, 90, "Kameo",
                     "Frames allowed for player 2's three Start taps.");

REXCVAR_DEFINE_BOOL(kameo_splitscreen_debug, false, "Kameo",
                    "Log split-screen player state once per second.");

// Momentary triggers: set true, the next frame performs the warp and clears
// them. Recovery for a player stuck on geometry.
REXCVAR_DEFINE_BOOL(kameo_gather_players, false, "Kameo",
                    "Teleport every other player to player 1, then self-clear.");
REXCVAR_DEFINE_BOOL(kameo_goto_player2, false, "Kameo",
                    "Teleport player 1 to player 2, then self-clear.");

// Manual override; normally the toggle above manages player 2 on its own.
REXCVAR_DEFINE_BOOL(kameo_join_player2, false, "Kameo",
                    "Force a player 2 (Kai) spawn now, then self-clear.");

// EXPERIMENTAL / CRASHES -- do not enable without expecting to lose the session.
//
// Traced with two crash stacks. Each subsystem that cached player-2 state has to
// be torn down, and the engine never does this, so there is no single call:
//   1. IconPlayerCheckTextures resolved player_list[1] to a freed object
//      -> fixed by clearing the slot before the kill (kept, it was a real bug).
//   2. IconPlayerCheckHealth still crashes: it caches the play-status pointer at
//      icon+1056 and uses it UNGUARDED after the null check --
//        ObjPlayCheckHealthUpgrade(*(icon+1056));
//        ObjPlayGetTrueHitPointPlayStatus(*(icon+1056));   <- access violation
//      so the HUD object itself must be removed, not just unlinked.
// Further consumers are likely behind that one.
//
// Kameo has no drop-out path. player_list is written only by objectLevelInit,
// objectKillAll and PlayerReady, and the game never destroys one player alone.
// Its own "character is dying" routine, morphPrepareObjectToDie, does NOT call
// objectKill at all:
//
//     v4 = charData[668];
//     v4[295] = fadeoutprocess;      // install a fade-out process
//     *v4  &= 0xFEFFFFFF;            // clear a state bit
//     v4[1] |= 0x80;                 // mark dying
//     cameraKillCameraBelongingTo(charData[2]);
//
// i.e. the object is flagged to fade out and destroys itself later, through its
// own tick. Reproducing that needs the character struct layout (offsets 668,
// 649, 70, ...) which we do not have types for yet -- forcing it by hand is how
// the last three attempts crashed. Default off: disabling split-screen collapses
// the view and leaves Kai standing; reload the level to remove him.
REXCVAR_DEFINE_BOOL(kameo_splitscreen_kill_p2, false, "Kameo",
                    "EXPERIMENTAL: destroy player 2 when disabling split-screen "
                    "(known to crash; reload the level instead).");

// Guest routines used by the teleport. TU addresses from tu_names_final.json.
#ifdef KAMEO_TU
extern "C" REX_FUNC(__imp__sub_82260A38);  // objectAskObject
extern "C" REX_FUNC(__imp__sub_82261830);  // objectWarpObjectPosAngle
extern "C" REX_FUNC(__imp__sub_82287960);  // cameraGetActiveCamera
extern "C" REX_FUNC(__imp__sub_822888A8);  // cameraSetupDefaultBlend
extern "C" REX_FUNC(__imp__sub_82288A80);  // cameraSetBlend
extern "C" REX_FUNC(__imp__sub_8227D208);  // levelCreatePlayerSetup
extern "C" REX_FUNC(__imp__sub_8225F878);  // _objectKill
extern "C" REX_FUNC(__imp__sub_82287348);  // cameraKillCameraBelongingTo
extern "C" REX_FUNC(__imp__sub_82261180);  // objectGetActiveObj
#define KAMEO_OBJECT_ASK_OBJECT __imp__sub_82260A38
#define KAMEO_WARP_POS_ANGLE __imp__sub_82261830
#define KAMEO_CAM_GET_ACTIVE __imp__sub_82287960
#define KAMEO_CAM_DEFAULT_BLEND __imp__sub_822888A8
#define KAMEO_CAM_SET_BLEND __imp__sub_82288A80
#define KAMEO_CREATE_PLAYER_SETUP __imp__sub_8227D208
#define KAMEO_OBJECT_KILL __imp__sub_8225F878
#define KAMEO_CAM_KILL_OWNED __imp__sub_82287348
#define KAMEO_GET_ACTIVE_OBJ __imp__sub_82261180
#else
extern "C" REX_FUNC(__imp__sub_82251DF8);  // objectAskObject
extern "C" REX_FUNC(__imp__sub_82252BE0);  // objectWarpObjectPosAngle
extern "C" REX_FUNC(__imp__sub_822777F8);  // cameraGetActiveCamera
extern "C" REX_FUNC(__imp__sub_82278740);  // cameraSetupDefaultBlend
extern "C" REX_FUNC(__imp__sub_82278918);  // cameraSetBlend
extern "C" REX_FUNC(__imp__sub_8226D6A8);  // levelCreatePlayerSetup
extern "C" REX_FUNC(__imp__sub_82250C58);  // _objectKill
extern "C" REX_FUNC(__imp__sub_822771E0);  // cameraKillCameraBelongingTo
extern "C" REX_FUNC(__imp__sub_82252530);  // objectGetActiveObj
extern "C" REX_FUNC(__imp__sub_821A8B10);  // KaiForceMorph
extern "C" REX_FUNC(__imp__sub_8217CB88);  // kaiSackGetName
extern "C" REX_FUNC(__imp__sub_8217C958);  // kaiSackPopulateSack
#define KAMEO_SACK_POPULATE __imp__sub_8217C958
#define KAMEO_FORCE_MORPH __imp__sub_821A8B10
#define KAMEO_SACK_GET_NAME __imp__sub_8217CB88
#define KAMEO_HAVE_MORPH 1
#define KAMEO_OBJECT_ASK_OBJECT __imp__sub_82251DF8
#define KAMEO_WARP_POS_ANGLE __imp__sub_82252BE0
#define KAMEO_CAM_GET_ACTIVE __imp__sub_822777F8
#define KAMEO_CAM_DEFAULT_BLEND __imp__sub_82278740
#define KAMEO_CAM_SET_BLEND __imp__sub_82278918
#define KAMEO_CREATE_PLAYER_SETUP __imp__sub_8226D6A8
#define KAMEO_OBJECT_KILL __imp__sub_82250C58
#define KAMEO_CAM_KILL_OWNED __imp__sub_822771E0
#define KAMEO_GET_ACTIVE_OBJ __imp__sub_82252530
#endif

namespace {

// objectAskObject message ids, read out of triggerWarpPlayersBehindTrigger and
// objectWarpAllPlayers.
constexpr uint32_t kMsgGetWarpableObject = 0x689298A;  // player -> warpable obj
constexpr uint32_t kMsgGetPosition = 0x1235863;        // obj -> float[3] out
constexpr uint32_t kMsgCameraPostWarp = 0xFA23B6E;     // camera settle
constexpr uint32_t kMsgKaiForceMorph = 8541368;        // kaiAsk -> KaiForceMorph

// Same table the combat hooks call kPlayerRootTable.
#ifdef KAMEO_TU
constexpr uint32_t kPlayerList = 0x828683D8;
constexpr uint32_t kNumPlayers = 0x82BCEE8C;
constexpr uint32_t kIsSplitScreenMode = 0x82BCE070;
constexpr uint32_t kNumWantedPlayers = 0x8278C1AC;
constexpr uint32_t kCameraArray = 0x828E1FA8;
// levelCreatePlayerSetup stores its setup_no here; PlayerReady reuses it for the
// next player. TU address derived with scripts/find_data_reloc.py (single site).
constexpr uint32_t kCurrentSetupPoint = 0x82785364;
constexpr uint32_t kTextureMemResult = 0;   // not mapped on TU
constexpr uint32_t kTextureMemResult0 = 0;
constexpr uint32_t kBackBufferTexture = 0;
constexpr uint32_t kNextLevelPlayerObject = 0x827AA15C;
#else
constexpr uint32_t kPlayerList = 0x8280D1A0;
constexpr uint32_t kNumPlayers = 0x82B718FC;
constexpr uint32_t kIsSplitScreenMode = 0x82B70AF8;
constexpr uint32_t kNumWantedPlayers = 0x8273A880;
constexpr uint32_t kCameraArray = 0x82885530;
constexpr uint32_t kCurrentSetupPoint = 0x827338D4;
// Render-target bookkeeping read by depthEffects_render. Watched only to see
// whether repeated viewport-count changes exhaust texture memory.
constexpr uint32_t kTextureMemResult = 0x82B70A80;
constexpr uint32_t kTextureMemResult0 = 0x82B70A88;
constexpr uint32_t kBackBufferTexture = 0x82758880;
constexpr uint32_t kNextLevelPlayerObject = 0x82755BE8;
constexpr uint32_t kPlayerControllers = 0x82732F48;
constexpr uint32_t kSticks = 0x8276706C;
constexpr uint32_t kCommitTestButtonMask = 0x82731F54;
#endif

// cameraGetActiveCamera(i) walks camera[37 * i]; 37 dwords = 148 bytes per
// player slot. A non-null head means that player's cameras have been built.
constexpr uint32_t kCameraStride = 37 * 4;

// Set once we raise the gate, so turning the cvar off restores the game's own
// state instead of leaving it latched.
std::atomic<uint32_t> g_forcing{0};

// Set while player 2 is parked. Collapsing the view by dropping numWantedPlayers
// also stops kameoControllerTick polling his pad, which unbinds his controller:
// he could not tap to rejoin, and even a cvar rejoin left him with no input.
// Instead keep him a fully bound player and force only the VIEWPORT COUNT
// register inside mainSetupMainRenderInfo.
std::atomic<uint32_t> g_collapse_view{0};

// Sacks captured from MorphValid, which RECEIVES a valid character object. The
// previous approach walked player_list -> +0x200 -> +2680 -> +548 blind every
// frame and kept faulting on transient garbage (1152, 0x3F800000, an unmapped
// heap pointer, 0x3F5E708B). No range check can separate a live pointer from a
// dead one, so stop deriving them and only use values the game gave us.
std::atomic<uint32_t> g_sack_p1{0};
std::atomic<uint32_t> g_sack_p2{0};

// Last player-2 object we know about. The opt-in kill path clears player_list[1]
// but can leave the Kai object alive in the world; without this the next spawn
// adds another body next to the old one (and again, and again).
uint32_t g_last_player2 = 0;
bool ParkPlayerObject(uint8_t* base, uint32_t player2);


uint32_t AskObject(PPCContext& src, uint8_t* base, uint32_t obj, uint32_t msg,
                   uint32_t arg) {
  PPCContext c = src;
  c.r3.u64 = obj;
  c.r4.u64 = 0;
  c.r5.u64 = msg;
  c.r6.u64 = arg;
  KAMEO_OBJECT_ASK_OBJECT(c, base);
  return c.r3.u32;
}

// player_list[i] is a player wrapper. Resolving it to something warpable must go
// through whatever object is CURRENTLY active for that player, because Kameo
// spends most of the game morphed into a warrior and the base player object is
// not the thing standing in the world.
//
// kMsgGetWarpableObject (used by objectWarpAllPlayers) answers for the plain
// form but comes back null while morphed, which made rejoin silently fail and
// leave player 2 at the parked spot. The game's own code -- triggerWarpPlayers-
// BehindTrigger, IconPlayerCheckHealth -- always consults objectGetActiveObj
// first, so do the same and keep the message as a fallback.
uint32_t ActiveObjectFor(PPCContext& src, uint8_t* base, uint32_t player) {
  if (player == 0) {
    return 0;
  }
  PPCContext c = src;
  c.r3.u64 = player;
  KAMEO_GET_ACTIVE_OBJ(c, base);
  return c.r3.u32;
}

uint32_t WarpableObjectFor(PPCContext& src, uint8_t* base, uint32_t player,
                           uint32_t scratch_out) {
  if (player == 0) {
    return 0;
  }
  const uint32_t active = ActiveObjectFor(src, base, player);
  REX_STORE_U32(scratch_out, 0);
  AskObject(src, base, player, kMsgGetWarpableObject, scratch_out);
  const uint32_t warpable = REX_LOAD_U32(scratch_out);
  const uint32_t chosen = active ? active : (warpable ? warpable : player);
  if (REXCVAR_GET(kameo_splitscreen_debug)) {
    REXLOG_INFO("[splitscreen]   resolve player {:08X}: active={:08X} warpable={:08X} -> {:08X}",
                player, active, warpable, chosen);
  }
  return chosen;
}

// Moves player `dst_idx` onto player `src_idx`'s position, then re-blends the
// moved player's camera so it does not lerp across the whole level.
// Reads a guest float, applies `delta`, writes it back. The guest is big-endian
// but REX_LOAD_U32/REX_STORE_U32 already normalise, so a plain bit-cast is right.
void AddGuestFloat(uint8_t* base, uint32_t addr, float delta) {
  (void)base;
  uint32_t bits = REX_LOAD_U32(addr);
  float v;
  std::memcpy(&v, &bits, sizeof(v));
  v += delta;
  std::memcpy(&bits, &v, sizeof(bits));
  REX_STORE_U32(addr, bits);
}

bool WarpPlayerToPlayer(PPCContext& ctx, uint8_t* base, uint32_t dst_idx,
                        uint32_t src_idx) {
  const uint32_t dst_player = REX_LOAD_U32(kPlayerList + dst_idx * 4);
  const uint32_t src_player = REX_LOAD_U32(kPlayerList + src_idx * 4);
  if (dst_player == 0 || src_player == 0) {
    return false;
  }

  // Reserve a frame below the caller's stack so the guest calls we make push
  // their own frames *below* our scratch instead of over it.
  const uint32_t frame = (ctx.r1.u32 - 0x200) & ~0xFu;
  const uint32_t scratch_ptr = frame + 0x40;    // one guest pointer
  const uint32_t scratch_pos = frame + 0x50;    // float[4] position
  const uint32_t scratch_blend = frame + 0x70;  // camera blend descriptor

  PPCContext call = ctx;
  call.r1.u64 = frame;

  if (REXCVAR_GET(kameo_splitscreen_debug)) {
    REXLOG_INFO("[splitscreen] warp p{}->p{}: dst_player={:08X} src_player={:08X}",
                dst_idx + 1, src_idx + 1, dst_player, src_player);
  }
  const uint32_t dst_obj = WarpableObjectFor(call, base, dst_player, scratch_ptr);
  const uint32_t src_obj = WarpableObjectFor(call, base, src_player, scratch_ptr);
  if (dst_obj == 0 || src_obj == 0) {
    REXLOG_WARN("[splitscreen]   abort: dst_obj={:08X} src_obj={:08X}", dst_obj, src_obj);
    return false;
  }

  for (uint32_t i = 0; i < 4; ++i) {
    REX_STORE_U32(scratch_pos + i * 4, 0);
  }
  AskObject(call, base, src_obj, kMsgGetPosition, scratch_pos);

  // Position query. kMsgGetPosition returns -1 on objects that do not answer it
  // -- notably Kameo's warrior forms -- leaving the buffer zeroed, which warped
  // player 2 to the world origin. triggerWarpPlayersBehindTrigger handles this
  // with a vtable fallback, so do the same:
  //     if (objectAskObject(obj, 0, 0x1235863, &pos) == -1)
  //         (*(*obj + 136))(obj[1], &pos);
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
        // REX_CALL_INDIRECT_FUNC expands against whatever `ctx` is in scope, so
        // shadow the parameter with our prepared context -- otherwise the getter
        // would run on the caller's registers instead of r3=obj[1], r4=&pos.
        {
          PPCContext& ctx = g;
          REX_CALL_INDIRECT_FUNC(getter);
        }
      }
      if (REXCVAR_GET(kameo_splitscreen_debug)) {
        REXLOG_INFO("[splitscreen]   pos fallback vtbl+136={:08X} -> [{:08X} {:08X} {:08X}]",
                    getter, REX_LOAD_U32(scratch_pos + 0),
                    REX_LOAD_U32(scratch_pos + 4), REX_LOAD_U32(scratch_pos + 8));
      }
    } else {
      if (REXCVAR_GET(kameo_splitscreen_debug)) {
        REXLOG_INFO("[splitscreen]   pos from {:08X}: rc={} raw=[{:08X} {:08X} {:08X}]",
                    src_obj, rc, REX_LOAD_U32(scratch_pos + 0),
                    REX_LOAD_U32(scratch_pos + 4), REX_LOAD_U32(scratch_pos + 8));
      }
    }
  }

  // Nudge sideways so the arriving player does not land inside the target.
  // Position is float[3]; X is index 0.
  const float offset =
      static_cast<float>(REXCVAR_GET(kameo_splitscreen_rejoin_offset));
  if (offset != 0.0f) {
    AddGuestFloat(base, scratch_pos + 0, offset);
  }

  // Null angle => objectWarpObjectPosAngle skips the orientation set and keeps
  // the player facing wherever they already were.
  PPCContext w = call;
  w.r3.u64 = dst_obj;
  w.r4.u64 = scratch_pos;
  w.r5.u64 = 0;
  KAMEO_WARP_POS_ANGLE(w, base);
  if (REXCVAR_GET(kameo_splitscreen_debug)) {
    REXLOG_INFO("[splitscreen]   warped {:08X} to [{:08X} {:08X} {:08X}]", dst_obj,
                REX_LOAD_U32(scratch_pos + 0), REX_LOAD_U32(scratch_pos + 4),
                REX_LOAD_U32(scratch_pos + 8));
  }

  PPCContext cam = call;
  cam.r3.u64 = dst_idx;
  KAMEO_CAM_GET_ACTIVE(cam, base);
  const uint32_t active_cam = cam.r3.u32;
  if (active_cam != 0) {
    PPCContext b = call;
    b.r3.u64 = dst_idx;
    b.r4.u64 = scratch_blend;
    KAMEO_CAM_DEFAULT_BLEND(b, base);
    REX_STORE_U32(scratch_blend, 1);  // blend mode used by the trigger warp

    PPCContext s = call;
    s.r3.u64 = dst_idx;
    s.r4.u64 = scratch_blend;
    s.r5.u64 = active_cam;
    KAMEO_CAM_SET_BLEND(s, base);

    AskObject(call, base, active_cam, kMsgCameraPostWarp, 0);
  }
  return true;
}

// Spawns player 2 into the running level, mirroring the call PlayerReady makes
// when chaining players at level init:
//   levelCreatePlayerSetup(currentSetupPoint, "Kai", numPlayers)
// PlayerReady then registers the new object into player_list and bumps
// numPlayers, and the camera for that slot is created as part of the spawn.
bool SpawnPlayer2(uint8_t* base) {
  if (REX_LOAD_U32(kPlayerList + 0) == 0) {
    REXLOG_WARN("[splitscreen] join ignored: no level running");
    return false;
  }
  if (REX_LOAD_U32(kPlayerList + 4) != 0) {
    REXLOG_WARN("[splitscreen] join ignored: player 2 already present");
    return false;
  }

  PPCContext* ctx = CurrentGuestContext();
  if (!ctx) {
    return false;
  }

  // An earlier player 2 may still be standing in the level with no player_list
  // entry (the kill path orphans it). Move it out of sight before adding another
  // -- otherwise each toggle leaves another Kai where the last one stood.
  if (g_last_player2 != 0 && g_last_player2 != REX_LOAD_U32(kPlayerList + 4)) {
    if (ParkPlayerObject(base, g_last_player2)) {
      REXLOG_WARN("[splitscreen] parked orphaned player 2 {:08X} before respawn",
                  g_last_player2);
    }
    g_last_player2 = 0;
  }

  // The render path needs both of these before a second viewport is valid.
  REX_STORE_U32(kIsSplitScreenMode, 1);
  if (REX_LOAD_U32(kNumWantedPlayers) < 2) {
    REX_STORE_U32(kNumWantedPlayers, 2);
  }

  const uint32_t frame = (ctx->r1.u32 - 0x200) & ~0xFu;
  const uint32_t str_addr = frame + 0x40;
  WriteGuestStringAt(base, str_addr, "Kai");

  // Player 1 is spawned with nextLevelPlayerObject (level-dependent, e.g. Kameo);
  // PlayerReady's chain hardcodes "Kai" for the second player. Kai has no
  // warrior wheel, so optionally spawn player 2 as player 1's type instead.
  uint32_t objtype = str_addr;
  if (REXCVAR_GET(kameo_player2_as_kameo)) {
    // Address of the buffer, not its contents -- see KameoOverridePlayer2Type.
    if (REX_LOAD_U8(kNextLevelPlayerObject) != 0) {
      objtype = kNextLevelPlayerObject;
    }
  }

  PPCContext c = *ctx;
  c.r1.u64 = frame;
  c.r3.u64 = REX_LOAD_U32(kCurrentSetupPoint) & 0xFF;  // setup_no (u8)
  c.r4.u64 = objtype;                                  // "Kai" or player 1's type
  c.r5.u64 = REX_LOAD_U32(kNumPlayers) & 0xFF;         // newplrno
  KAMEO_CREATE_PLAYER_SETUP(c, base);

  REXLOG_INFO("[splitscreen] requested player 2 spawn (setup={} plrno={} objtype={:08X})",
              REX_LOAD_U32(kCurrentSetupPoint) & 0xFF,
              REX_LOAD_U32(kNumPlayers) & 0xFF, objtype);
  return true;
}

// Removes player 2. There is no drop-out path in the game -- player_list is only
// written by objectLevelInit, objectKillAll and PlayerReady -- but the object
// destruction handler clears player_list[playerNo] for us:
//     if (player_list[obj->playerNo] == obj) player_list[obj->playerNo] = 0;
// so killing the object unregisters it. numPlayers is not decremented there;
// the pin below restores it once the slot reads back empty.
// Removing a player is staged across frames, because several subsystems cache
// state derived from it and the engine has no single teardown call.
//
// Stage 1 - unpublish: clear player_list[1], drop numPlayers/numWantedPlayers and
//   isSplitScreenMode. That makes mainSetupMainRenderInfo see a different
//   viewport count, and it then does
//       numRenderViewports = v27;
//       objectAskClass(0x6FA5E, 0, 0xB838885);
//   i.e. the game's own "player count changed" message to the HUD class, which
//   is what lets the player-2 icon tear itself down.
// Stage 2 - after a few frames, kill the cameras and then the object.
//
// Doing both in one frame is what crashed: the HUD ticked once more holding a
// cached play-status pointer (icon+1056, used unguarded by IconPlayerCheckHealth)
// that referred to the freed player.
int g_despawn_stage = 0;   // 0 = idle, >0 = frames since unpublish

bool DespawnPlayer2(uint8_t* base) {
  static uint32_t s_pending_player2 = 0;
  uint32_t player2 = REX_LOAD_U32(kPlayerList + 4);
  if (player2 == 0) {
    player2 = s_pending_player2;   // stage 1 already cleared the slot
  }
  if (player2 == 0) {
    g_despawn_stage = 0;
    return false;
  }
  s_pending_player2 = player2;
  PPCContext* ctx = CurrentGuestContext();
  if (!ctx) {
    return false;
  }

  if (g_despawn_stage == 0) {
    // Stage 1: unpublish and let the HUD reconfigure.
    REX_STORE_U32(kPlayerList + 4, 0);
    if (REX_LOAD_U32(kNumPlayers) > 1) {
      REX_STORE_U32(kNumPlayers, 1);
    }
    g_collapse_view.store(1, std::memory_order_release);
    g_despawn_stage = 1;
    REXLOG_INFO("[splitscreen] despawn stage 1: unpublished player 2 {:08X}", player2);
    return false;  // not finished; the caller keeps the pending latch
  }

  if (++g_despawn_stage < 5) {
    return false;  // let the HUD/viewport reconfigure settle
  }

  const uint32_t frame = (ctx->r1.u32 - 0x200) & ~0xFu;
  const uint32_t owner = REX_LOAD_U32(player2 + 8);
  if (owner != 0) {
    PPCContext k = *ctx;
    k.r1.u64 = frame;
    k.r3.u64 = owner;
    KAMEO_CAM_KILL_OWNED(k, base);
  }
  PPCContext c = *ctx;
  c.r1.u64 = frame;
  c.r3.u64 = player2;
  KAMEO_OBJECT_KILL(c, base);
  g_despawn_stage = 0;
  REXLOG_INFO("[splitscreen] despawn stage 2: killed {:08X} (cam owner {:08X})",
              player2, owner);
  return true;
}

// Safe alternative to destroying player 2.
//
// Killing the object corrupts the heap: the teardown reaches
//   audioMainTick -> audioTick -> CCue::Destroy -> ~CCue
//   -> CSound::RegistryHostShutdown -> LocalFree -> RtlpCoalesceFreeBlocks
// i.e. the XACT audio system frees memory the dead player still co-owned. The
// engine has no drop-out path, so out-of-band destruction is simply unsafe.
//
// Instead keep Kai alive and move him far outside the play area, then pin
// numPlayers to 1 so triggerTrigActiveInq only requires player 1. He is invisible
// and inert, costs a little memory, and nothing is freed.
bool ParkPlayerObject(uint8_t* base, uint32_t player2) {
  if (player2 == 0) {
    return false;
  }
  PPCContext* ctx = CurrentGuestContext();
  if (!ctx) {
    return false;
  }

  const uint32_t frame = (ctx->r1.u32 - 0x200) & ~0xFu;
  const uint32_t scratch_ptr = frame + 0x40;
  const uint32_t scratch_pos = frame + 0x50;

  PPCContext call = *ctx;
  call.r1.u64 = frame;

  const uint32_t obj = WarpableObjectFor(call, base, player2, scratch_ptr);
  if (obj == 0) {
    return false;
  }

  // -100000.0f on Y, well under any level.
  const uint32_t kFarBelow = 0xC7C35000u;
  REX_STORE_U32(scratch_pos + 0, 0);
  REX_STORE_U32(scratch_pos + 4, kFarBelow);
  REX_STORE_U32(scratch_pos + 8, 0);
  REX_STORE_U32(scratch_pos + 12, 0);

  PPCContext w = call;
  w.r3.u64 = obj;
  w.r4.u64 = scratch_pos;
  w.r5.u64 = 0;
  KAMEO_WARP_POS_ANGLE(w, base);
  g_collapse_view.store(1, std::memory_order_release);
  REXLOG_INFO("[splitscreen] parked player object {:08X} out of play", player2);
  return true;
}

bool ParkPlayer2(uint8_t* base) {
  return ParkPlayerObject(base, REX_LOAD_U32(kPlayerList + 4));
}

// Brings a parked player 2 back: restores the player count, warps him onto
// player 1 and re-blends his camera (WarpPlayerToPlayer already does the camera
// work). No object is created or destroyed, so none of the teardown hazards
// apply -- this is just the teleport we already use for "gather players".
bool UnparkPlayer2(PPCContext& ctx, uint8_t* base) {
  if (REX_LOAD_U32(kPlayerList + 4) == 0) {
    return false;
  }
  // Restore the counts first: both were dropped to 1 while he was parked, and
  // the gameplay and render sides need to see two players again before he is
  // back in play.
  //
  // Raising numWantedPlayers here is safe even though the usual rule is "only
  // before any player has spawned": that rule exists because the viewport count
  // must not reach 2 before player 2's camera is built. Parking never destroyed
  // him or his camera, so both already exist. Without this the viewport stays at
  // 1 and he comes back invisible -- which is why re-enabling only appeared to
  // work when kill_p2 forced a full respawn through level init.
  if (REX_LOAD_U32(kNumPlayers) < 2) {
    REX_STORE_U32(kNumPlayers, 2);
  }
  REX_STORE_U32(kIsSplitScreenMode, 1);
  if (REX_LOAD_U32(kNumWantedPlayers) < 2) {
    REX_STORE_U32(kNumWantedPlayers, 2);
  }
  if (!WarpPlayerToPlayer(ctx, base, 1, 0)) {
    return false;
  }
  g_collapse_view.store(0, std::memory_order_release);
  REXLOG_INFO("[splitscreen] unparked player 2 back to player 1");
  return true;
}

#ifdef KAMEO_HAVE_MORPH
// The character object -- the one whose [2680] is valid -- is the player_list
// entry plus 0x200. Confirmed from MorphValid's receiver for both players:
//   p1 41F8B2B0 -> 41F8B4B0,  p2 4203BC00 -> 4203BE00.
constexpr uint32_t kCharObjOffset = 0x200;

// player_list[i] + 0x200 is the character object in steady state, but during
// level transitions the slot can hold a partially initialised value. A null
// check is not enough: a small non-pointer (observed: 1152) sailed through and
// was dereferenced at +548, faulting on guest 0x6A4. Guest heap objects live
// around 0x4xxxxxxx and statics at 0x82xxxxxx, so anything below 16MB is junk.
bool PlausibleGuestPtr(uint32_t v) {
  return v >= 0x01000000u && v < 0xC0000000u;
}

// The warrior sacks live in the image's static data (observed 0x82804058 and
// 0x82806358; the module maps 0x82000000-0x82BF0000). A loose range check is not
// enough here -- during the battlefield transition the field held 0x3F800000,
// which is the float 1.0, passed the generic test, and faulted at +0x22E8.
bool PlausibleSackPtr(uint32_t v) {
  return v >= 0x82000000u && v < 0x82C00000u;
}

// MorphValid(obj, slot) reads:
//   data = *(obj + 2680);  sack = *(data + 548);
//   warriorIdx = *(u16*)(sack + 2 * (slot + 4468));   // FFFF => refuse
// so mirroring player 1's three slots into player 2's sack lets his wheel commit.
void SyncPlayer2Warriors(uint8_t* base) {
  if (!REXCVAR_GET(kameo_player2_morph)) {
    return;
  }
  const uint32_t sack1 = g_sack_p1.load(std::memory_order_acquire);
  const uint32_t sack2 = g_sack_p2.load(std::memory_order_acquire);
  if (!PlausibleSackPtr(sack1) || !PlausibleSackPtr(sack2) || sack1 == sack2) {
    return;  // not captured yet, or not both players
  }

  bool changed = false;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint32_t off = 2 * (slot + 4468);
    const uint16_t src = static_cast<uint16_t>(REX_LOAD_U16(sack1 + off));
    const uint16_t dst = static_cast<uint16_t>(REX_LOAD_U16(sack2 + off));
    if (src != dst) {
      REX_STORE_U16(sack2 + off, src);
      changed = true;
    }
  }
  if (changed && REXCVAR_GET(kameo_splitscreen_debug)) {
    REXLOG_INFO("[splitscreen] synced warriors -> p2 sack {:08X} [{:04X} {:04X} {:04X}]",
                sack2, REX_LOAD_U16(sack2 + 2 * (0 + 4468)),
                REX_LOAD_U16(sack2 + 2 * (1 + 4468)),
                REX_LOAD_U16(sack2 + 2 * (2 + 4468)));
  }
}

#endif

// Same read the warrior wheel uses:
//   pad     = playerControllers[36 * playerNo]
//   base    = *Sticks                       (Sticks is a pointer global)
//   buttons = *(base + ((pad + ROL(pad,2)) << 6)) & commitTestButtonMask
// `raw_pad` reads a physical pad slot directly instead of going through the
// player -> pad mapping. That matters for the opt-in toggle: while player 2 is
// parked we pin numPlayers/numWantedPlayers to 1, the game stops polling him,
// and playerControllers[36*1] goes stale -- so his taps were never seen and he
// could leave but not rejoin.
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

// Three Start presses inside the window toggle kameo_story_splitscreen, which
// the lifecycle code below then acts on (spawn / park / unpark as appropriate).
void ServicePlayer2TripleTap(uint8_t* base) {
  if (!REXCVAR_GET(kameo_p2_triple_tap)) {
    return;
  }
  // Physical pad 1: works whether or not player 2 is currently an active player.
  const uint32_t buttons = ReadPadButtons(base, 1, /*raw_pad=*/true);
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
    if (REXCVAR_GET(kameo_splitscreen_debug)) {
      REXLOG_INFO("[splitscreen] p2 tap {} (buttons={:08X})", s_taps + 1, buttons);
    }
    if (s_taps == 0) {
      s_frames = 0;
    }
    if (++s_taps >= 3) {
      const bool now = !REXCVAR_GET(kameo_story_splitscreen);
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

}  // namespace

#ifdef KAMEO_TU
constexpr uint32_t kPlayerListPub = 0x828683D8;
#else
constexpr uint32_t kPlayerListPub = 0x8280D1A0;
#endif

#ifdef KAMEO_TU
constexpr uint32_t kNextLevelPlayerObjectPub = 0x827AA15C;
#else
constexpr uint32_t kNextLevelPlayerObjectPub = 0x82755BE8;
#endif

// Entry of mainSetupMainRenderInfo. Read-only with respect to game state: it
// services teleport requests and optionally logs. It must not write any of the
// split-screen globals -- see the file header.
void KameoForceStorySplitScreen() {
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  ServicePlayer2TripleTap(base);
  ServiceTeleportRequests(base);
#ifdef KAMEO_HAVE_MORPH
  SyncPlayer2Warriors(base);
#endif

  if (REXCVAR_GET(kameo_join_player2)) {
    REXCVAR_SET(kameo_join_player2, false);
    SpawnPlayer2(base);
  }

  // The toggle owns player 2's lifecycle. BOTH directions are asynchronous:
  // spawning reports in via PlayerReady some frames later, and _objectKill only
  // queues the object for deletion -- player_list[1] stays populated until the
  // destruction handler runs. Each direction therefore issues exactly one
  // request and then waits for the slot to change. Without the despawn latch the
  // kill re-fired the next frame on an already-killed object and crashed.
  static bool s_spawn_pending = false;
  static bool s_parked = false;
  const bool want_split = REXCVAR_GET(kameo_story_splitscreen);
  const bool level_running = REX_LOAD_U32(kPlayerList + 0) != 0;
  const bool have_p2 = REX_LOAD_U32(kPlayerList + 4) != 0;

  if (want_split) {
    g_despawn_stage = 0;
    if (have_p2) {
      s_spawn_pending = false;
      g_last_player2 = REX_LOAD_U32(kPlayerList + 4);
      // He is alive but parked out of play -- bring him back rather than
      // spawning a second Kai on top of the first.
      if (s_parked) {
        PPCContext* c = CurrentGuestContext();
        if (c && UnparkPlayer2(*c, base)) {
          s_parked = false;
        }
      }
    } else {
      s_parked = false;
      if (level_running && !s_spawn_pending) {
        s_spawn_pending = SpawnPlayer2(base);
      }
    }
  } else {
    s_spawn_pending = false;
    if (have_p2) {
      if (REXCVAR_GET(kameo_splitscreen_kill_p2) || g_despawn_stage != 0) {
        DespawnPlayer2(base);          // opt-in, corrupts the heap -- see above
      } else if (!s_parked) {
        s_parked = ParkPlayer2(base);  // default: move him out of play
      }
      // Keep gameplay single-player while he is parked, so triggers still fire.
      if (s_parked && REX_LOAD_U32(kNumPlayers) > 1) {
        REX_STORE_U32(kNumPlayers, 1);
      }
    } else {
      s_parked = false;
    }
  }

  if (REXCVAR_GET(kameo_story_splitscreen)) {
    // Safe to set any time: this is what makes the *next* level init build the
    // second player's cameras. It does not by itself change the viewport count.
    REX_STORE_U32(kIsSplitScreenMode, 1);

    // numWantedPlayers is what mainSetupMainRenderInfo turns into the viewport
    // count, and raising it mid-level makes mainRender walk a viewport whose
    // camera was never built -- the "invalid function at 0x00008000" crash.
    //
    // Player 2's camera only appears *after* player 2 spawns, which itself needs
    // this value raised, so waiting for the camera deadlocks. The safe window is
    // before any player has spawned (menu / level load): raise it there and the
    // level builds both players and both cameras in the right order.
    const bool players_spawned = REX_LOAD_U32(kPlayerList + 0) != 0;
    if (!players_spawned && REX_LOAD_U32(kNumWantedPlayers) < 2) {
      REX_STORE_U32(kNumWantedPlayers, 2);
    }
    // Gameplay reads numPlayers; triggerTrigActiveInq only completes a trigger
    // when it reaches index numPlayers-1, so a phantom second player stalls the
    // story. Pin it while no real player 2 has registered.
    if (REX_LOAD_U32(kPlayerList + 4) == 0 && REX_LOAD_U32(kNumPlayers) > 1) {
      REX_STORE_U32(kNumPlayers, 1);
    }
    g_forcing.store(1, std::memory_order_release);
  } else if (g_forcing.exchange(0, std::memory_order_acq_rel) != 0) {
    // numWantedPlayers is left alone deliberately -- see g_collapse_view.
    // Deliberately do NOT clear isSplitScreenMode here. Dropping it tears down
    // per-viewport render state (effectsRender -> _depthEffects_render then
    // called through an unregistered pointer on the frame the split came back).
    // The view already collapses via numWantedPlayers, and the save/storybook
    // gates keep the flag from affecting saves, so leaving it set is safe.
  }

  if (REXCVAR_GET(kameo_splitscreen_debug)) {
    static uint32_t tick = 0;
    if ((tick++ % 60) == 0) {
      REXLOG_INFO(
          "[splitscreen] numPlayers={} wanted={} players=[{:08X} {:08X}] "
          "cameras=[{:08X} {:08X}]",
          REX_LOAD_U32(kNumPlayers), REX_LOAD_U32(kNumWantedPlayers),
          REX_LOAD_U32(kPlayerList + 0), REX_LOAD_U32(kPlayerList + 4),
          REX_LOAD_U32(kCameraArray + 0), REX_LOAD_U32(kCameraArray + kCameraStride));
      if (kTextureMemResult != 0) {
        REXLOG_INFO("[splitscreen]   texmem={:08X}/{:08X} backbuf={:08X}",
                    REX_LOAD_U32(kTextureMemResult), REX_LOAD_U32(kTextureMemResult0),
                    REX_LOAD_U32(kBackBufferTexture));
      }
    }
  }
}

// Both gates run immediately after `lwz r11, isSplitScreenMode` at a site whose
// following `bne` decides save/profile behaviour. Forcing r11 to 0 makes those
// paths take the single-player branch, so the campaign progress block is written
// and the profile is not reported corrupt -- without disturbing the global that
// the camera system needs. Only active while we are the ones forcing the split.
void KameoSaveGameSplitGate(PPCRegister& r11) {
  if (REXCVAR_GET(kameo_story_splitscreen)) {
    r11.u64 = 0;
  }
}

void KameoStorybookSplitGate(PPCRegister& r11) {
  if (REXCVAR_GET(kameo_story_splitscreen)) {
    r11.u64 = 0;
  }
}

// PlayerReady chains the next player with a hardcoded object type:
//     addi r4, r10, aKai@l        ; "Kai"
//     bl   levelCreatePlayerSetup
// Player 1 meanwhile is spawned with nextLevelPlayerObject (the level's own
// character, i.e. Kameo). Kai has no warrior wheel because warriors belong to
// Kameo, so swapping r4 here -- after the string is loaded, before the call --
// gives player 2 the same character type as player 1.
//
// This is the path the game itself uses at level init, which is why overriding
// only our own SpawnPlayer2 had no effect.
void KameoOverridePlayer2Type(PPCRegister& r4) {
  if (!REXCVAR_GET(kameo_player2_as_kameo)) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  // nextLevelPlayerObject is a char BUFFER, not a pointer to one -- levelCreate-
  // PlayerSetup receives its address. Loading a dword from it and passing that
  // fed the ASCII of the name in as a pointer ("Kame" -> 0x4B616D65).
  if (REX_LOAD_U8(kNextLevelPlayerObjectPub) != 0) {
    r4.u64 = kNextLevelPlayerObjectPub;
    REXLOG_INFO("[splitscreen] player 2 objtype overridden -> {:08X}",
                kNextLevelPlayerObjectPub);
  }
}

// Probe on kaiAsk's entry. When player 1 morphs, this reports the exact object
// and argument block the game passes -- the ground truth we need instead of
// guessing which object should receive message 8541368.
void KameoProbeKaiAsk(PPCRegister& r3, PPCRegister& r5, PPCRegister& r6) {
  if (!REXCVAR_GET(kameo_splitscreen_debug)) {
    return;
  }
  if (r5.u32 != 8541368u) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  const uint32_t argbuf = (r6.u32 + 7) & ~7u;
  REXLOG_INFO("[splitscreen] PROBE kaiAsk: recv={:08X} msg={:08X} args={:08X} name={:08X} "
              "| p1={:08X} p2={:08X}",
              r3.u32, r5.u32, r6.u32, REX_LOAD_U32(argbuf + 4),
              REX_LOAD_U32(kPlayerListPub + 0), REX_LOAD_U32(kPlayerListPub + 4));
}

// Probe on MorphValid's entry. It receives the object whose [2680] is valid --
// the thing we have repeatedly failed to identify -- plus the slot being tested.
// Logs the resolved sack and the warrior index stored for that slot, so we can
// see exactly why player 2 is refused (expected: -1 = no warrior assigned).
void KameoProbeMorphValid(PPCRegister& r3, PPCRegister& r4) {
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  const uint32_t obj = r3.u32;
  const uint32_t slot = r4.u32 & 0xFF;
  const uint32_t data = obj ? REX_LOAD_U32(obj + 2680) : 0;
  const uint32_t sack = data ? REX_LOAD_U32(data + 548) : 0;
  uint32_t idx = 0xFFFF;
  if (sack && slot <= 2) {
    idx = REX_LOAD_U16(sack + 2 * (slot + 4468));
  }
  // Publish the sack for whichever player this object belongs to. r3 is handed
  // to us by the game, so this chain is safe here even though deriving it from
  // player_list every frame was not.
  if (PlausibleSackPtr(sack)) {
    const uint32_t p1 = REX_LOAD_U32(kPlayerListPub + 0);
    const uint32_t p2 = REX_LOAD_U32(kPlayerListPub + 4);
    if (p1 != 0 && obj == p1 + 0x200) {
      g_sack_p1.store(sack, std::memory_order_release);
    } else if (p2 != 0 && obj == p2 + 0x200) {
      g_sack_p2.store(sack, std::memory_order_release);
    }
  }
  if (!REXCVAR_GET(kameo_splitscreen_debug)) {
    return;
  }
  REXLOG_INFO("[splitscreen] PROBE MorphValid: obj={:08X} slot={} data={:08X} "
              "sack={:08X} warriorIdx={:04X} | p1={:08X} p2={:08X}",
              obj, slot, data, sack, idx,
              REX_LOAD_U32(kPlayerListPub + 0), REX_LOAD_U32(kPlayerListPub + 4));
}

// Entry of kaiSackPopulate(a1). Adds every warrior to the sack before the game
// runs its eventDone-gated adds, which then find them already present.
void KameoUnlockAllWarriors(PPCRegister& r3) {
  if (!REXCVAR_GET(kameo_unlock_all_warriors)) {
    return;
  }
  uint8_t* base = GuestBase();
  PPCContext* ctx = CurrentGuestContext();
  if (!base || !ctx || r3.u32 == 0) {
    return;
  }

  static const char* kWarriors[] = {
      "FlowerBoxer", "Spitter",     "EarthRubble", "EarthArmadillo",
      "WaterCreatureSmall", "WaterCannon", "SnowMan", "IceYeti",
      "FireAnt",     "FireDragon"};

  const uint32_t frame = (ctx->r1.u32 - 0x200) & ~0xFu;
  const uint32_t str_addr = frame + 0x40;
  for (const char* name : kWarriors) {
    WriteGuestStringAt(base, str_addr, name);
    PPCContext c = *ctx;
    c.r1.u64 = frame;
    c.r3.u64 = r3.u32;
    c.r4.u64 = str_addr;
    KAMEO_SACK_POPULATE(c, base);
  }
  REXLOG_INFO("[splitscreen] unlocked all warriors for sack owner {:08X}", r3.u32);
}

// Runs between `lwz r11, isSplitScreenMode` and the store of the viewport count.
// r10 is that count. Forcing it to 1 collapses the split without touching
// numWantedPlayers, so player 2 stays a bound, polled player while parked.
void KameoViewportGate(PPCRegister& r10, PPCRegister& r11) {
  (void)r11;
  if (g_collapse_view.load(std::memory_order_acquire) != 0) {
    r10.u64 = 1;
  }
}

// depthEffects_render dispatches through a table:
//     lbz  r10, 192(r11)      ; index byte
//     rotlwi r9, r10, 2       ; * 4
//     lwzx r8, r9, r26        ; table[idx]
//     mtctr r8 ; bctrl
// After a viewport-count change the index/table can be stale, so r8 comes back
// as data rather than code -- the values the runtime trapped on (0x00004000,
// 0x00010000, 0x00020000, 0x00080000) are all single bits, i.e. flags read past
// the end of the table. Divert anything that is not a plausible code address to
// a bare `blr`, so the frame drops one effect instead of killing the process.
void KameoGuardEffectsDispatch(PPCRegister& r8) {
  const uint32_t target = r8.u32;
  // .text spans 0x820B0000-0x826CF6BC; BINK code runs to 0x826DF9F8.
  if (target >= 0x820B0000u && target < 0x826E0000u) {
    return;
  }
  r8.u64 = 0x823BB178u;  // a function consisting solely of `blr`
  if (REXCVAR_GET(kameo_splitscreen_debug)) {
    REXLOG_WARN("[splitscreen] effects dispatch target {:08X} invalid -> no-op", target);
  }
}
