#pragma once

#include <string>

namespace kameo::ui {

// Renames a save everywhere the game looks: the name and file name inside the save data, the folder named by the name's hash, and the content header the save list reads.
bool RenameSaveGame(const std::string& file_name, const std::u16string& name);

}  // namespace kameo::ui
