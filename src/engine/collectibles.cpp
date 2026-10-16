#include "engine/collectibles.h"

#include <algorithm>
#include <string>

#include "engine/current_level.h"
#include "engine/guest_addresses.h"

namespace kameo::engine {
namespace {

// eventSetEx's table of {event, score} pairs.
constexpr uint32_t kEventScoreTable = 0x827324F0;
constexpr uint32_t kEventScoreCount = 135;
constexpr uint32_t kFallbackPrice = 50000;
// {u64 score, float time, name, xuid} per score level.
constexpr uint32_t kScoreBank = 0x8289B458;
constexpr uint32_t kScoreBankStride = 40;
constexpr uint32_t kScoreLevelCount = 12;
constexpr uint64_t kScoreCap = 0x174876E7FFull;

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

uint32_t CollectiblePrice(uint8_t* base, const Collectible& item) {
  for (uint32_t i = 0; i < kEventScoreCount; ++i) {
    if (REX_LOAD_U32(kEventScoreTable + 8 * i) == item.event) return REX_LOAD_U32(kEventScoreTable + 8 * i + 4);
  }
  return kFallbackPrice;
}

uint64_t BankedScore(uint8_t* base) {
  uint64_t total = 0;
  for (uint32_t i = 0; i < kScoreLevelCount; ++i) total += std::min(REX_LOAD_U64(kScoreBank + kScoreBankStride * i), kScoreCap);
  return std::min(total, kScoreCap);
}

void SpendBankedScore(uint8_t* base, uint64_t amount) {
  while (amount > 0) {
    uint32_t largest = 0;
    for (uint32_t i = 1; i < kScoreLevelCount; ++i) {
      if (REX_LOAD_U64(kScoreBank + kScoreBankStride * i) > REX_LOAD_U64(kScoreBank + kScoreBankStride * largest)) largest = i;
    }
    const uint64_t banked = REX_LOAD_U64(kScoreBank + kScoreBankStride * largest);
    if (banked == 0) return;
    const uint64_t taken = std::min(banked, amount);
    REX_STORE_U64(kScoreBank + kScoreBankStride * largest, banked - taken);
    amount -= taken;
  }
}

}  // namespace kameo::engine
