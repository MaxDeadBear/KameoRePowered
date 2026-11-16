// Mods: RexFabric (Lua and native mods from the mods folder beside the game) and the `kameo` API the game gives them.
//
// The app calls these in order; each does nothing in a build without RexFabric (KAMEO_MODS off).
#pragma once

#include <cstdint>

#include <rex/result.h>

namespace rex {
class SettingsRegistry;
}
namespace rex::ui {
class IUISystem;
}
struct PPCContext;

// From OnConfigure, before any settings source is read.
rex::Result<void> KameoModsConfigure(rex::SettingsRegistry& settings);
// From OnInit, with the window open and after the Guide is installed: the mods folder, the keys, the `kameo` API
// and the mods menu.
rex::Result<void> KameoModsInit(rex::ui::IUISystem& ui);
// From OnTitleLaunch: loads the enabled mods. The title is loaded and has not run yet.
void KameoModsStart();
// Before the title unloads: stops every mod and removes its hooks. Later calls do nothing.
void KameoModsStop();
// Once per game tick, on the game thread: runs what mods asked for that needs the game thread, and fires the frame
// and level events.
void KameoModsTick(PPCContext& ctx, uint8_t* base);
// A mod window (the mods menu, the node editor) is open and wants the keyboard and mouse.
bool KameoModsMenuShown();
// Any script or native mod is switched on. They can change anything in the game, so scores stay off the leaderboards
// while one is.
bool KameoModsEnabled();
