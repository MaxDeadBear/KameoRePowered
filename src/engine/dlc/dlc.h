// The Kameo skin hot-swap's game-thread side.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/ppc.h>

// The English name of a downloadable Kameo skin for a string id only the title update's string tables carry, or of the
// shader mod's costume; empty for any other id.
std::u16string KameoDlcSkinName(uint32_t string_id);

namespace kameo::engine {

// Loads a skin queued by the model dialog through objectAlternativeSetFileInUse; called from the per-frame game tick.
void ProcessPendingDlcSwap(PPCContext& ctx, uint8_t* base);

// Notes which skin Kameo and each warrior is wearing, from the per-frame game tick.
void TickKameoSkin(uint8_t* base);

// Each shader mod made for a character's standard look (Kameo's everyday outfit) adds a costume of its own after their
// last skin: that look with the mod's shading. The GPU side lists them, by model type and in file order, whenever the
// mods change.
void SetShaderSkins(std::vector<std::pair<uint32_t, std::u16string>> costumes);
// The position among that character's costumes of the one chosen, or -1. Readable from any thread.
int ShaderSkinChosen(uint32_t model_type);
// Whether the character wears it now: chosen, and no skin model loaded over their standard look.
bool WearsShaderSkin(uint32_t model_type);
// While set, a skin line's next-skin call steps back one skin instead. Game thread only.
void SetCostumeStepBack(bool back);
// The entry of objectAlternativeSetFileInUse: a costume is saved as its own suffix, which loads as the standard look
// with the costume chosen, and any other suffix leaves it.
void SettingSkin(uint8_t* base, uint32_t model_type, PPCRegister& r4, PPCRegister& r1);

}  // namespace kameo::engine
