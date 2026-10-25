// The achievement unlock notice: a Kameo text box at the top of the screen, since the SDK's ImGui toast is built on a drawer this app leaves null and the game itself showed the console's.
#pragma once

#include <cstdint>

#include <rex/system/achievement_manager.h>

struct PPCContext;

// From any thread: queues the notice for the game thread to show.
void KameoPushAchievementToast(const rex::system::AchievementEvent& event);
// Once per game tick: creates, updates and retires the text box.
void KameoTickAchievementToast(PPCContext& ctx, uint8_t* base);
// levelNameTextWindowReady arriving with a window this toast asked for; true when it was taken.
bool KameoClaimAchievementToastWindow(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t owner);
// Whether a creation is in flight, so the FPS counter and the toast never both wait for the same ready callback.
bool KameoAchievementToastPending();
