// The Kameo skin hot-swap's game-thread side.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc.h>

// The English name of a downloadable Kameo skin for a string id only the title update's string tables carry, or of the
// shader mod's costume; empty for any other id.
std::u16string KameoDlcSkinName(uint32_t string_id);

namespace kameo::engine {

// Loads a skin queued by the model dialog through objectAlternativeSetFileInUse; called from the per-frame game tick.
void ProcessPendingDlcSwap(PPCContext& ctx, uint8_t* base);

// Notes which skin Kameo is wearing, from the per-frame game tick.
void TickKameoSkin(uint8_t* base);

// Each shader mod made for her everyday outfit adds a Bonus costume of its own, after the last one: that outfit with
// the mod's look. The GPU side names them, in file order, whenever the mods change.
void SetKameoShaderSkins(std::vector<std::u16string> names);
// The position in that list of the costume chosen, or -1. Readable from any thread.
int KameoShaderSkinChosen();
// Whether she wears it now: chosen, and no costume model loaded over her everyday outfit.
bool KameoWearsShaderSkin();
// While set, the Bonus costume line's next-costume call steps back one costume instead. Game thread only.
void SetKameoCostumeStepBack(bool back);
// The entry of objectAlternativeSetFileInUse for Kameo: the costume is saved as this suffix, which loads as her
// everyday outfit with the costume chosen, and any other suffix leaves it.
void KameoSettingSkin(uint8_t* base, PPCRegister& r4, PPCRegister& r1);

}  // namespace kameo::engine
