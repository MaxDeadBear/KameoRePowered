// Costumes made with the Blender add-on: copies of the game's skin models, each listed as a costume of its own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <rex/ppc.h>

namespace kameo::engine {

// Gathers costumes/ beside the game into a package of its own under the content root, before the game mounts its
// downloadable content; with no costumes left, takes the package away.
void BuildModCostumePackage(const std::filesystem::path& user_data_root);

// A costume's name by its skin model ("007183BF_m1A2B3C"), or empty for any other skin.
std::u16string ModCostumeName(std::string_view model);
// The string id the Bonus page names it by, or 0 for any other skin; and the name for such an id, or empty.
uint32_t ModCostumeStringId(std::string_view model);
std::u16string ModCostumeNameOfString(uint32_t string_id);

// Copies a costume file written again into the package and reloads it on a character wearing it; from the game tick.
void TickModCostumes(PPCContext& ctx, uint8_t* base);

}  // namespace kameo::engine
