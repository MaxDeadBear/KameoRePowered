#pragma once

#include "core/guest.h"

namespace kameo::engine {

// Holds the game's Upgrade All Warriors cheat while kameo_unlock_all_warriors is on, and gives a running level's players every warrior when it is switched on.
void TickWarriorUnlock(PPCContext& ctx, uint8_t* base);

}  // namespace kameo::engine
