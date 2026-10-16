// Unlock cheats, each revealed by a pause-menu code: every elemental warrior and move up front, and every mirror unlockable treated as earned.

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "core/hooks.h"
#include "core/unlock_codes.h"
#include "engine/guest_addresses.h"
#include "engine/unlocks.h"

// kaiSackPopulate adds each warrior only once its story event is done (eventDone(0x2B) -> "FlowerBoxer", ...); adding them all first makes those adds no-ops.
REXCVAR_DEFINE_BOOL(kameo_unlock_all_warriors, false, "Kameo",
                    "Give every elemental warrior and every warrior move immediately.");

REXCVAR_DEFINE_BOOL(kameo_unlock_all_extras, false, "Kameo", "Treat every mirror unlockable as earned: the cheats, costumes, concept art, FMVs and replay tools.");

namespace {

constexpr const char* kWarriors[] = {
    "FlowerBoxer", "Spitter",     "EarthRubble", "EarthArmadillo",
    "WaterCreatureSmall", "WaterCannon", "SnowMan", "IceYeti",
    "FireAnt",     "FireDragon"};

// The mirror's unlockables table (0x82732928, 42 rows of event, level, score and text) names events 0x1E5-0x20E, and every cheat, costume and extra is gated on eventDone for one of them.
constexpr uint32_t kFirstMirrorUnlockEvent = 0x1E5;
constexpr uint32_t kLastMirrorUnlockEvent = 0x20E;

// The game's own Upgrade All Warriors cheat, which every warrior move check passes on; like the game's other cheats it stops Battle Points while it is on.
constexpr uint32_t kEventMonsterUpgradeAll = 0x82B71CE4;
// Each player's warrior sack, the one kaiSackPopulate fills.
constexpr uint32_t kFirstWarriorSack = 0x82804058;
constexpr uint32_t kWarriorSackBytes = 8960;

bool WarriorsWanted() { return REXCVAR_GET(kameo_unlock_all_warriors) && KameoCodeUnlocked(KameoUnlockCode::Warriors); }

void AddAllWarriors(PPCContext& ctx, uint8_t* base, uint32_t sack) {
  const uint32_t frame = kameo::engine::ScratchFrame(ctx.r1.u32);
  const uint32_t name = frame + 0x40;
  for (const char* warrior : kWarriors) {
    WriteGuestStringAt(base, name, warrior);
    PPCContext c = ctx;
    c.r1.u64 = frame;
    c.r3.u64 = sack;
    c.r4.u64 = name;
    kameo::engine::KaiSackPopulateSack(c, base);
  }
}

}  // namespace

namespace kameo::engine {

void TickWarriorUnlock(PPCContext& ctx, uint8_t* base) {
  static bool applied = false;
  const bool wanted = WarriorsWanted();
  // Loading a save restores the game's cheat record, so the cheat is held every tick rather than set once.
  if (wanted) REX_STORE_U32(kEventMonsterUpgradeAll, 1);
  if (wanted == applied) return;
  applied = wanted;
  if (!wanted) {
    REX_STORE_U32(kEventMonsterUpgradeAll, 0);
    return;
  }
  // Sacks are only filled when a level builds the player, so a level already running gets the warriors here.
  for (uint32_t player = 0; player < 2; ++player) {
    if (!REX_LOAD_U32(kPlayerList + 4 * player)) continue;
    AddAllWarriors(ctx, base, kFirstWarriorSack + player * kWarriorSackBytes);
    REXLOG_INFO("[kameo] gave player {} every warrior", player + 1);
  }
}

}  // namespace kameo::engine

// Entry of kaiSackPopulate(a1).
void KameoUnlockAllWarriors(PPCRegister& r3) {
  if (!WarriorsWanted()) {
    return;
  }
  uint8_t* base = GuestBase();
  PPCContext* ctx = CurrentGuestContext();
  if (!base || !ctx || r3.u32 == 0) {
    return;
  }
  AddAllWarriors(*ctx, base, r3.u32);
}

// eventDone(event)
extern "C" REX_FUNC(sub_822CC3C0) {
  const uint32_t event = ctx.r3.u32;
  if (event >= kFirstMirrorUnlockEvent && event <= kLastMirrorUnlockEvent && REXCVAR_GET(kameo_unlock_all_extras) && KameoCodeUnlocked(KameoUnlockCode::GameCheats)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_822CC3C0(ctx, base);
}
