// DLC model files on the host: naming, the content folder they live in, and KameoDLCList.txt sync.
#pragma once

#include <filesystem>
#include <string>

// True for a Kameo DLC model file for title 0x007183BF, e.g. "007183BF_xmas1.mdl".
bool IsKameoDlcModelName(const std::string& filename);
// The DLC content folder holding one subfolder per package, under the user data root the runtime resolved.
std::filesystem::path KameoActiveDlcPath(const std::filesystem::path& user_data_root);
// Appends custom model files a package folder holds but its KameoDLCList.txt does not list.
void SyncKameoDlcListForCustomModels(const std::filesystem::path& user_data_root);
