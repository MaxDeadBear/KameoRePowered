// The Kameo skin hot-swap's game-thread side.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

// The English name of a downloadable Kameo skin for a string id only the title update's string tables carry, or null.
const char16_t* KameoDlcSkinName(uint32_t string_id);

namespace kameo::engine {

// Loads a skin queued by the model dialog through objectAlternativeSetFileInUse; called from the per-frame game tick.
void ProcessPendingDlcSwap(PPCContext& ctx, uint8_t* base);

}  // namespace kameo::engine
