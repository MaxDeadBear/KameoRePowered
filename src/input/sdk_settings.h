// The SDK's keyboard and mouse settings that our options pages and mouse code read and write (hid.mnk_mouse,
// hid.mnk_sensitivity). They belong to the input system, so they are reached through it rather than declared here.
#pragma once

#include <rex/setting.h>
#include <rex/types.h>

rex::Setting<bool>& MnkMouse();
rex::Setting<rex::f64>& MnkSensitivity();
