// Combat cheats: infinite energy and health, a score bonus and one-shot damage for base Kameo.

#include "core/hooks.h"
#include "engine/cheats.h"
#include "engine/guest_addresses.h"

std::atomic<uint32_t> g_kameo_infinite_energy_enabled{0};
std::atomic<uint32_t> g_kameo_infinite_health_enabled{0};

using namespace kameo::engine;

namespace {

// Returns true if `health_record` belongs to one of the active player actors.
bool IsPlayerHealthRecord(uint8_t* base, uint32_t health_record) {
  if (health_record == 0) {
    return false;
  }

  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t player_root = REX_LOAD_U32(kPlayerList + i * 4);
    if (player_root == 0) {
      continue;
    }

    const uint32_t actor = REX_LOAD_U32(player_root + 0x10);
    if (actor != 0 && REX_LOAD_U32(actor + 0xA24) == health_record) {
      return true;
    }
  }

  const uint32_t owner = REX_LOAD_U32(health_record);
  if (owner == 0) {
    return false;
  }

  PPCContext* ctx_ptr = CurrentGuestContext();
  if (!ctx_ptr) {
    return false;
  }

  PPCContext call_ctx = *ctx_ptr;
  call_ctx.r3.u64 = owner;
  ObjectIsPlayer(call_ctx, base);
  return call_ctx.r3.u32 != 0;
}

// Returns true if `attacker` belongs to the player while in base Kameo form.
bool IsPlayerKameoAttacker(uint8_t* base, uint32_t attacker) {
  if (attacker == 0 || !base) {
    return false;
  }
  const uint32_t player_root = REX_LOAD_U32(kPlayerList);
  if (player_root == 0) {
    return false;
  }
  const uint32_t player_actor = REX_LOAD_U32(player_root + 0x10);
  if (player_actor == 0) {
    return false;
  }

  const uint32_t owner = REX_LOAD_U32(attacker + 4);
  if (owner == player_actor || owner == player_root) {
    return true;
  }

  PPCContext* ctx_ptr = CurrentGuestContext();
  if (ctx_ptr) {
    PPCContext call_ctx = *ctx_ptr;
    call_ctx.r3.u64 = attacker;
    ObjectIsPlayer(call_ctx, base);
    if (call_ctx.r3.u32 != 0 && (owner == 0 || owner == player_actor)) {
      return true;
    }
  }
  return false;
}

// Calls the game's unlock-check function for `id` and returns the boolean result.
bool CallUnlockCheck(uint32_t id, PPCContext& source_ctx, uint8_t* base) {
  PPCContext call_ctx = source_ctx;
  call_ctx.r3.u64 = id;
  EventDone(call_ctx, base);
  return call_ctx.r3.u32 != 0;
}

// Returns the original max energy for the current player based on unlocked upgrades.
int32_t OriginalEnergyMax(PPCContext& source_ctx, uint8_t* base) {
  if (CallUnlockCheck(1159, source_ctx, base)) {
    return 999;
  }
  if (CallUnlockCheck(1158, source_ctx, base)) {
    return 700;
  }
  if (CallUnlockCheck(1157, source_ctx, base)) {
    return 400;
  }
  if (CallUnlockCheck(1156, source_ctx, base)) {
    return 200;
  }
  return 100;
}

// Returns the current energy value for the first player, or 0 if unavailable.
int32_t OriginalEnergyCurrent(PPCContext& source_ctx, uint8_t* base) {
  const uint32_t player_root = REX_LOAD_U32(kPlayerList);
  if (player_root == 0) {
    return 0;
  }

  const uint32_t actor = REX_LOAD_U32(player_root + 0x10);
  if (actor == 0) {
    return 0;
  }

  const uint32_t energy_record = REX_LOAD_U32(actor + 0xA24);
  if (energy_record == 0) {
    return 0;
  }

  PPCContext call_ctx = source_ctx;
  call_ctx.r3.u64 = energy_record;
  ObjPlayGetTrueHitPointPlayStatus(call_ctx, base);
  if (call_ctx.r3.u32 == 0) {
    return 0;
  }

  return static_cast<int32_t>(REX_LOAD_U32(call_ctx.r3.u32 + 0x18));
}

void RestoreOriginalEnergyCurrent(PPCRegister& r3) {
  PPCContext* ctx_ptr = CurrentGuestContext();
  uint8_t* base = GuestBase();
  if (!ctx_ptr || !base) {
    r3.s64 = 0;
    return;
  }
  r3.s64 = OriginalEnergyCurrent(*ctx_ptr, base);
}

void RestoreOriginalEnergyMax(PPCRegister& r3) {
  PPCContext* ctx_ptr = CurrentGuestContext();
  uint8_t* base = GuestBase();
  if (!ctx_ptr || !base) {
    r3.s64 = 100;
    return;
  }
  r3.s64 = OriginalEnergyMax(*ctx_ptr, base);
}

// Shared helper for KameoRefillMeterFloat and KameoRefillMeterFloatPlus4.
// Copies one float word from guest address (src.u32 + off) to (dst.u32 + off).
void RefillMeterAt(PPCRegister& dst, PPCRegister& src, uint32_t off) {
  if (g_kameo_infinite_energy_enabled.load(std::memory_order_acquire) == 0) {
    return;
  }

  if (dst.u32 == 0 || src.u32 == 0) {
    return;
  }

  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  REX_STORE_U32(dst.u32 + off, REX_LOAD_U32(src.u32 + off));
}

}  // namespace

void KameoInfiniteEnergy(PPCRegister& r3) {
  if (g_kameo_infinite_energy_enabled.load(std::memory_order_acquire) == 0) {
    return;
  }

  r3.s64 = 999;
}

void KameoInfiniteEnergyCurrent(PPCRegister& r3) {
  if (g_kameo_infinite_energy_enabled.load(std::memory_order_acquire) == 0) {
    RestoreOriginalEnergyCurrent(r3);
    return;
  }

  r3.s64 = 999;
}

void KameoInfiniteEnergyMax(PPCRegister& r3) {
  if (g_kameo_infinite_energy_enabled.load(std::memory_order_acquire) == 0) {
    RestoreOriginalEnergyMax(r3);
    return;
  }

  r3.s64 = 999;
}

void KameoRefillMeterFloat(PPCRegister& dst, PPCRegister& src) {
  RefillMeterAt(dst, src, 0);
}

void KameoRefillMeterFloatPlus4(PPCRegister& dst, PPCRegister& src) {
  RefillMeterAt(dst, src, 4);
}

void KameoRefillHealth(PPCRegister& r29) {
  if (r29.u32 == 0) {
    return;
  }

  if (g_kameo_infinite_health_enabled.load(std::memory_order_acquire) == 0) {
    return;
  }

  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  if (!IsPlayerHealthRecord(base, r29.u32)) {
    return;
  }

  REX_STORE_U32(r29.u32 + 0x0C, REX_LOAD_U32(r29.u32 + 0x2C));
}

std::atomic<int64_t> g_kameo_add_score_pending{0};

void KameoAddScore(PPCRegister& r11) {
  int64_t pending = g_kameo_add_score_pending.exchange(0, std::memory_order_acq_rel);
  if (pending == 0) {
    return;
  }
  int64_t new_total = static_cast<int64_t>(r11.s64) + pending;
  if (new_total < 0) {
    new_total = 0;
  }
  constexpr int64_t kScoreCap = 0x174876E7FFLL;
  if (new_total > kScoreCap) {
    new_total = kScoreCap;
  }
  r11.s64 = new_total;
}

std::atomic<uint32_t> g_kameo_kameo_oneshot_enabled{0};

void KameoOneShotDamage(PPCRegister& r27, PPCRegister& f31) {
  if (g_kameo_kameo_oneshot_enabled.load(std::memory_order_acquire) == 0) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  if (IsPlayerKameoAttacker(base, r27.u32)) {
    f31.f64 = 99999.0;
  }
}
