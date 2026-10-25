// What the pause menu tells the rest of the game, and what the rest of the game asks of it.
#pragma once

#include <cstdint>
#include <vector>

struct PPCContext;

// The player whose pause menu is open, or -1: the menu takes input only from them.
int KameoPausedBy();

// The pages F4 and F6 open: the in-theme Options and Achievements, in the pause menu, which is opened first in gameplay when it is not already.
enum class KameoMenuHotkey { Options, Achievements };
// From any thread (the UI frame callback presses the keys).
void KameoRequestMenuPage(KameoMenuHotkey which);
// Once per game tick, on the game thread.
void KameoTickMenuHotkeys(PPCContext& ctx, uint8_t* base);

// What the Achievements page is showing, for the icons drawn beside its rows: one entry per selectable row, in order.
struct KameoAchievementIconRow {
  uint32_t achievement_id = 0;
  bool unlocked = false;
};
// False unless the Achievements page is the one last built; then the text object of its window and its rows.
bool KameoAchievementsPageShowing(uint32_t* text_object, std::vector<KameoAchievementIconRow>* rows);
