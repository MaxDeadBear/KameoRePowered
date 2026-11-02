// Kameo RePowered launcher: the game's title update, which kameorepowered-tu.exe plays with (Expert mode, Rune Battle,
// Time Attack and the update's fixes). It lives in assets\TU: the update's delta patch (default.xexp) with a copy of
// default.xex beside it, which is where that build loads the game from, and the update's per-language text folders.

#pragma once

#include <filesystem>
#include <string>

namespace rex::filesystem {
class StfsContainerDevice;
}

namespace kameo::launcher {

inline constexpr const wchar_t* kTitleUpdateExe = L"kameorepowered-tu.exe";

// Whether the update is in place and the build that plays it is next to the launcher.
bool TitleUpdateInstalled(const std::filesystem::path& game_folder);

// Whether this release has the build that plays the update; a release made without the update's patch has none, and
// then the update has nothing to run it.
bool TitleUpdateBuildPresent(const std::filesystem::path& game_folder);

// Whether a package is a title update: an installer package whose root holds default.xexp.
bool IsTitleUpdatePackage(rex::filesystem::StfsContainerDevice& device);

// Unpacks a title-update package into assets\TU, once its patch is checked against this copy's default.xex (the update
// came in regional variants, each for its own release of the game) and against the patch kameorepowered-tu.exe was
// recompiled from. message says what happened either way.
bool InstallTitleUpdate(rex::filesystem::StfsContainerDevice& device, const std::filesystem::path& game_folder, std::string& message);

// Takes the update out again, so the launcher starts the vanilla build.
bool RemoveTitleUpdate(const std::filesystem::path& game_folder, std::string& error);

}  // namespace kameo::launcher
