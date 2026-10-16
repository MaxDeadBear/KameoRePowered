// The cheat switches the model dialog sets and the cheat hooks in engine/cheats.cpp read.
#pragma once

#include <atomic>
#include <cstdint>

extern std::atomic<uint32_t> g_kameo_infinite_energy_enabled;
extern std::atomic<uint32_t> g_kameo_infinite_health_enabled;
extern std::atomic<int64_t> g_kameo_add_score_pending;
extern std::atomic<uint32_t> g_kameo_kameo_oneshot_enabled;
