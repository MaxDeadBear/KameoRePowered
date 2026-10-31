// Keeps cheated scores off the leaderboards, and an accidental cheat from costing a player theirs: the saves are copied just before a cheat first takes effect, and scores stay off until the launcher puts that copy back.
#pragma once

#include <filesystem>
#include <string>

// Call once the settings are loaded, with the folder saves live under.
void KameoInitCheatGuard(const std::filesystem::path& content_root);

// Call when the settings may have changed. The first time a cheat is on, backs up the saves and marks them cheated.
void KameoNoteCheatSettings();

// Why leaderboard scores can't be posted now; empty when they can.
std::string KameoLeaderboardBlock();
