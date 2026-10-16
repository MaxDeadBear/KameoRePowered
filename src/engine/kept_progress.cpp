#include "engine/kept_progress.h"

#include <rex/logging.h>

#include "engine/guest_addresses.h"

namespace kameo::engine {
namespace {

// The four tables eventRememberEventsDone (0x822CC058) snapshots, as {address, rows, stride}.
struct EventTable {
  uint32_t address;
  uint32_t rows;
  uint32_t stride;
};
constexpr EventTable kRememberedTables[] = {
    {0x82731F98, 114, 12},  // eventCostArray
    {0x82732928, 42, 24},   // eventScoreArray
    {0x82731F58, 8, 8},     // eventCollectableArray
    {0x82749CB8, 12, 8},    // healthUpgradeEvents
};
// eventSetEx awards an event's score only while this is clear.
constexpr uint32_t kDoingTaskStart = 0x82B71CD8;

}  // namespace

std::vector<uint32_t> DoneProgressEvents(PPCContext& ctx, uint8_t* base) {
  std::vector<uint32_t> done;
  for (const EventTable& table : kRememberedTables) {
    for (uint32_t row = 0; row < table.rows; ++row) {
      const uint32_t event = REX_LOAD_U32(table.address + row * table.stride);
      // The original, not the unlock cvars' override, so an unlock shown only while a cvar is on is not written into the save.
      PPCContext call = ctx;
      call.r3.u64 = event;
      EventDone(call, base);
      if (call.r3.u32 != 0) done.push_back(event);
    }
  }
  return done;
}

void RestoreProgressEvents(PPCContext& ctx, uint8_t* base, const std::vector<uint32_t>& events) {
  const uint32_t doing_task_start = REX_LOAD_U32(kDoingTaskStart);
  REX_STORE_U32(kDoingTaskStart, 1);
  for (uint32_t event : events) {
    PPCContext call = ctx;
    call.r3.u64 = event;
    call.r4.u64 = 1;
    call.r5.u64 = 0;
    sub_822CC508(call, base);
  }
  REX_STORE_U32(kDoingTaskStart, doing_task_start);
  PPCContext save = ctx;
  save.r3.u64 = 1;
  sub_820BBC08(save, base);
  REXLOG_INFO("[kameo] kept {} collectible, upgrade and unlock events across the task jump", events.size());
}

}  // namespace kameo::engine
