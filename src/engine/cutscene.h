// Cutscene and movie hooks: skipping, and starting the credits from the options.
#pragma once

#include <cstdint>

// When the options' Credits were started (steady-clock milliseconds, 0 for none): they go through the every-level "Death" fmv object, which fmvloaded renames to "Credits" if it loads soon after.
extern int64_t g_kameo_credits_requested_ms;
