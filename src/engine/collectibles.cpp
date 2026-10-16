#include "engine/collectibles.h"

#include <string>

#include "engine/current_level.h"
#include "engine/guest_addresses.h"

namespace kameo::engine {
namespace {

bool OriginalEventDone(PPCContext& ctx, uint8_t* base, uint32_t event) {
  PPCContext call = ctx;
  call.r3.u64 = event;
  EventDone(call, base);
  return call.r3.u32 != 0;
}

}  // namespace

uint32_t CurrentLevelToken() {
  uint32_t hash = 0;
  for (const char c : KameoCurrentLevelName()) {
    hash = (hash << 4) + uint8_t(c);
    const uint32_t high = hash & 0xF0000000u;
    if (high) hash ^= high >> 24;
    hash &= ~high;
  }
  return hash;
}

bool CollectibleFound(PPCContext& ctx, uint8_t* base, const Collectible& item) { return OriginalEventDone(ctx, base, item.event); }

bool CollectibleMissed(PPCContext& ctx, uint8_t* base, const Collectible& item) {
  if (item.source != CollectibleSource::Level || item.last_task < 0 || CollectibleFound(ctx, base, item)) return false;
  if (item.warrior_event >= 0 && !OriginalEventDone(ctx, base, uint32_t(item.warrior_event))) return false;
  PPCContext call = ctx;
  call.r3.u64 = uint32_t(item.last_task);
  sub_822CE8C0(call, base);
  return call.r3.u32 != 0;
}

void GrantCollectible(PPCContext& ctx, uint8_t* base, const Collectible& item) {
  const uint32_t doing_task_start = REX_LOAD_U32(kDoingTaskStart);
  REX_STORE_U32(kDoingTaskStart, 1);
  PPCContext call = ctx;
  call.r3.u64 = item.event;
  call.r4.u64 = 1;
  call.r5.u64 = 1;
  sub_822CC508(call, base);
  REX_STORE_U32(kDoingTaskStart, doing_task_start);
}

}  // namespace kameo::engine
