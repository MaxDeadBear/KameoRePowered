// Costumes made with the Blender add-on: copies of the game's skin models, each listed as a costume of its own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ppc/context.h>

namespace kameo::engine {

// Gathers costumes/ beside the game into a package of its own under the content root, before the game mounts its
// downloadable content; with no costumes left, takes the package away.
void BuildModCostumePackage(const std::filesystem::path& user_data_root);

// A costume's name by its skin model ("007183BF_m1A2B3C"), or empty for any other skin.
std::u16string ModCostumeName(std::string_view model);
// The string id the Bonus page names it by, or 0 for any other skin; and the name for such an id, or empty.
uint32_t ModCostumeStringId(std::string_view model);
std::u16string ModCostumeNameOfString(uint32_t string_id);

// A costume as the Models window shows it; read from any thread.
struct ModCostumeInfo {
  std::string file;   // "007183BF_m1A2B3C.mdl", in ModCostumeFolder()
  std::string model;  // "007183BF_m1A2B3C"
  std::string name;   // UTF-8
  uint32_t model_type = 0;
  bool listed = false;  // the game has listed it (new costumes only after a restart)
  bool worn = false;    // on its character, as of the last check (about every two seconds)
};
std::vector<ModCostumeInfo> ListModCostumes();
// costumes/ beside the executable.
std::filesystem::path ModCostumeFolder();
// Has the tick copy the costume's file into the package and reload it on the character wearing it, changed or not.
void RequestModCostumeReload(std::string_view model);
// Makes a costume from a copy of a game model: costumes/<type>_m<id>.mdl and its name in costumes.toml. Empty on
// success (the file name in *file_out), else what went wrong. It shows in the game after a restart.
std::string CreateModCostume(const std::filesystem::path& base_mdl, const std::string& name, std::string* file_out);

// Copies a costume file written again into the package and reloads it on a character wearing it; from the game tick.
void TickModCostumes(PPCContext& ctx, uint8_t* base);

}  // namespace kameo::engine
