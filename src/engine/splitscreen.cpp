// Story split-screen: the per-frame lifecycle that spawns, benches and restores player 2, and the gates that keep saves single-player.

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "core/hooks.h"
#include "engine/splitscreen_internal.h"

REXCVAR_DEFINE_BOOL(kameo_story_splitscreen, false, "Kameo",
                    "Force split-screen on in the story campaign.");

// PlayerReady's own chain spawns "Kai", who has no warrior wheel because warriors belong to Kameo.
REXCVAR_DEFINE_BOOL(kameo_player2_as_kameo, false, "Kameo",
                    "Spawn player 2 as player 1's character type instead of Kai "
                    "(gives access to the warrior wheel).");

// Manual override; normally the triple-tap toggle manages player 2 on its own.
REXCVAR_DEFINE_BOOL(kameo_join_player2, false, "Kameo",
                    "Force a player 2 (Kai) spawn now, then self-clear.");

namespace kameo::engine {

std::atomic<uint32_t> g_collapse_view{0};

namespace {

// Last player 2 we registered; the opt-in kill path can orphan him in the world, so he is parked before another spawns.
uint32_t g_last_player2 = 0;

// Mirrors PlayerReady's chain at level init: levelCreatePlayerSetup(currentSetupPoint, "Kai", numPlayers); PlayerReady then registers him and builds his camera.
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

  const uint32_t frame = ScratchFrame(ctx->r1.u32);
  const uint32_t str_addr = frame + 0x40;
  WriteGuestStringAt(base, str_addr, "Kai");

  uint32_t objtype = str_addr;
  if (REXCVAR_GET(kameo_player2_as_kameo)) {
    // The buffer's address, not its contents; see KameoOverridePlayer2Type.
    if (REX_LOAD_U8(kNextLevelPlayerObject) != 0) {
      objtype = kNextLevelPlayerObject;
    }
  }

  PPCContext c = *ctx;
  c.r1.u64 = frame;
  c.r3.u64 = REX_LOAD_U32(kCurrentSetupPoint) & 0xFF;  // setup_no (u8)
  c.r4.u64 = objtype;                                  // "Kai" or player 1's type
  c.r5.u64 = REX_LOAD_U32(kNumPlayers) & 0xFF;         // newplrno
  LevelCreatePlayerSetup(c, base);

  REXLOG_INFO("[splitscreen] requested player 2 spawn (setup={} plrno={} objtype={:08X})",
              REX_LOAD_U32(kCurrentSetupPoint) & 0xFF,
              REX_LOAD_U32(kNumPlayers) & 0xFF, objtype);
  return true;
}

}  // namespace
}  // namespace kameo::engine

using namespace kameo::engine;

// Entry of mainSetupMainRenderInfo: services the teleport and tap requests, then drives player 2 toward what kameo_story_splitscreen asks for.
void KameoSplitScreenTick() {
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  ServicePlayer2TripleTap(base);
  ServiceTeleportRequests(base);
  SyncPlayer2Warriors(base);

  if (REXCVAR_GET(kameo_join_player2)) {
    REXCVAR_SET(kameo_join_player2, false);
    SpawnPlayer2(base);
  }

  // Spawning reports in through PlayerReady frames later, so each direction issues one request and waits for player_list[1] to change.
  static bool s_spawn_pending = false;
  const bool want_split = REXCVAR_GET(kameo_story_splitscreen);
  const bool level_running = REX_LOAD_U32(kPlayerList + 0) != 0;
  const bool have_p2 = REX_LOAD_U32(kPlayerList + 4) != 0;
  // objectKillAll empties player_list before freeing every object, so no player 2 we remember outlives the level.
  if (!level_running) {
    ForgetBenchedPlayer();
    g_last_player2 = 0;
  }
  TickBench(base);

  if (want_split) {
    // kameoControllerTick wipes the input of every controller slot not marked bound, and unbinds slots past numWantedPlayers; only the front end binds them, so a player two added mid-level is bound here the same way.
    if (level_running && REX_LOAD_U32(kPlayerControllers + 36 + 4) == 0) {
      REX_STORE_U32(kPlayerControllers + 36 + 4, 1);
      REX_STORE_U32(kPlayerControllers + 36 + 8, 0);
      REXLOG_INFO("[splitscreen] bound player 2's controller slot");
    }
    if (have_p2) {
      s_spawn_pending = false;
      g_last_player2 = REX_LOAD_U32(kPlayerList + 4);
      // PlayerReady built his camera when it registered him, so his viewport can open.
      g_collapse_view.store(0, std::memory_order_release);
    } else if (Player2Benched()) {
      PPCContext* c = CurrentGuestContext();
      if (c) UnbenchPlayer2(*c, base);
    } else if (level_running && !s_spawn_pending) {
      s_spawn_pending = SpawnPlayer2(base);
    }
  } else {
    s_spawn_pending = false;
    if (have_p2) {
      BenchPlayer2(base);
    }
    // Keep gameplay single-player while he is benched, so triggers still fire.
    if (Player2Benched() && REX_LOAD_U32(kNumPlayers) > 1) REX_STORE_U32(kNumPlayers, 1);
  }

  // isSplitScreenMode is never cleared again: dropping it tears down per-viewport render state that crashes when the split comes back.
  if (REXCVAR_GET(kameo_story_splitscreen)) {
    REX_STORE_U32(kIsSplitScreenMode, 1);
    // Raising numWantedPlayers mid-level renders a viewport with no camera, so it is only raised before any player has spawned.
    const bool players_spawned = REX_LOAD_U32(kPlayerList + 0) != 0;
    if (!players_spawned && REX_LOAD_U32(kNumWantedPlayers) < 2) {
      REX_STORE_U32(kNumWantedPlayers, 2);
    }
    // triggerTrigActiveInq only completes a trigger at index numPlayers-1, so a phantom player 2 would stall the story.
    if (REX_LOAD_U32(kPlayerList + 4) == 0 && REX_LOAD_U32(kNumPlayers) > 1) {
      REX_STORE_U32(kNumPlayers, 1);
    }
  }
}

// PlayerReady(player, playerNo) chains levelCreatePlayerSetup("Kai") for the next player while numPlayers < numWantedPlayers, so with split-screen off a level load stops at player 1 instead of rebuilding player 2.
extern "C" REX_FUNC(sub_8226CE68) {
  if (!REXCVAR_GET(kameo_story_splitscreen) && ctx.r4.u32 == 0 && REX_LOAD_U32(kNumWantedPlayers) > 1) {
    REX_STORE_U32(kNumWantedPlayers, 1);
    REXLOG_INFO("[splitscreen] split-screen off: level loads with player 1 only");
  }
  __imp__sub_8226CE68(ctx, base);
}

// saveGameFileOpenedForWriting writes campaign progress only when !isSplitScreenMode, so the loaded flag is forced to 0 there.
void KameoSaveGameSplitGate(PPCRegister& r11) {
  if (REXCVAR_GET(kameo_story_splitscreen)) {
    r11.u64 = 0;
  }
}

// storybookCurrentGameDisplayCorrupt tests the same flag and would report the profile corrupt.
void KameoStorybookSplitGate(PPCRegister& r11) {
  if (REXCVAR_GET(kameo_story_splitscreen)) {
    r11.u64 = 0;
  }
}

// PlayerReady's `addi r4, aKai` before levelCreatePlayerSetup: player 2 takes player 1's type, which is the path level init uses.
void KameoOverridePlayer2Type(PPCRegister& r4) {
  if (!REXCVAR_GET(kameo_player2_as_kameo)) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  // Passing a dword loaded from the buffer fed the name's ASCII in as a pointer ("Kame" -> 0x4B616D65).
  if (REX_LOAD_U8(kNextLevelPlayerObject) != 0) {
    r4.u64 = kNextLevelPlayerObject;
    REXLOG_INFO("[splitscreen] player 2 objtype overridden -> {:08X}",
                kNextLevelPlayerObject);
  }
}

// Between `lwz r11, isSplitScreenMode` and the store of the viewport count in r10.
void KameoViewportGate(PPCRegister& r10, PPCRegister& r11) {
  (void)r11;
  if (g_collapse_view.load(std::memory_order_acquire) != 0) {
    r10.u64 = 1;
  }
}
