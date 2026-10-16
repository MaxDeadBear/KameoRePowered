// The collectibles, upgrades and unlocks that a task jump would otherwise lose.
#pragma once

#include <cstdint>
#include <vector>

#include "core/guest.h"

namespace kameo::engine {

// The done events of the game's own remembered tables: elemental fruit and warrior moves, Battle Point unlocks, Crystal Eyes and money pouches, health upgrades.
std::vector<uint32_t> DoneProgressEvents(PPCContext& ctx, uint8_t* base);
// Sets them again after taskStartTaskNoLevelChange has cleared every event, without scoring them a second time, then saves once.
void RestoreProgressEvents(PPCContext& ctx, uint8_t* base, const std::vector<uint32_t>& events);

}  // namespace kameo::engine
