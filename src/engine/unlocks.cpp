// Unlock cheats: every elemental warrior up front, and every mirror unlockable treated as earned.

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "core/hooks.h"
#include "engine/guest_addresses.h"

// kaiSackPopulate adds each warrior only once its story event is done (eventDone(0x2B) -> "FlowerBoxer", ...); adding them all first makes those adds no-ops.
REXCVAR_DEFINE_BOOL(kameo_unlock_all_warriors, false, "Kameo",
                    "Give every elemental warrior immediately.");

REXCVAR_DEFINE_BOOL(kameo_unlock_all_extras, false, "Kameo", "Treat every mirror unlockable as earned: the cheats, costumes, concept art, FMVs and replay tools.");

namespace {

constexpr const char* kWarriors[] = {
    "FlowerBoxer", "Spitter",     "EarthRubble", "EarthArmadillo",
    "WaterCreatureSmall", "WaterCannon", "SnowMan", "IceYeti",
    "FireAnt",     "FireDragon"};

// The mirror's unlockables table (0x82732928, 42 rows of event, level, score and text) names events 0x1E5-0x20E, and every cheat, costume and extra is gated on eventDone for one of them.
constexpr uint32_t kFirstMirrorUnlockEvent = 0x1E5;
constexpr uint32_t kLastMirrorUnlockEvent = 0x20E;

}  // namespace

// Entry of kaiSackPopulate(a1).
void KameoUnlockAllWarriors(PPCRegister& r3) {
  if (!REXCVAR_GET(kameo_unlock_all_warriors)) {
    return;
  }
  uint8_t* base = GuestBase();
  PPCContext* ctx = CurrentGuestContext();
  if (!base || !ctx || r3.u32 == 0) {
    return;
  }

  const uint32_t frame = kameo::engine::ScratchFrame(ctx->r1.u32);
  const uint32_t str_addr = frame + 0x40;
  for (const char* name : kWarriors) {
    WriteGuestStringAt(base, str_addr, name);
    PPCContext c = *ctx;
    c.r1.u64 = frame;
    c.r3.u64 = r3.u32;
    c.r4.u64 = str_addr;
    kameo::engine::KaiSackPopulateSack(c, base);
  }
  REXLOG_INFO("[splitscreen] unlocked all warriors for sack owner {:08X}", r3.u32);
}

// eventDone(event)
extern "C" REX_FUNC(sub_822CC3C0) {
  const uint32_t event = ctx.r3.u32;
  if (event >= kFirstMirrorUnlockEvent && event <= kLastMirrorUnlockEvent && REXCVAR_GET(kameo_unlock_all_extras)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_822CC3C0(ctx, base);
}
