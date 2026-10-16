#pragma once

#include <chrono>

namespace kameo::gpu {

// A high-resolution waitable timer on Windows, null elsewhere or when the system has none.
void* CreatePreciseTimer();
void ClosePreciseTimer(void* timer);

// Sleeps on `timer` (or a plain sleep without one) until a millisecond before `deadline` and spins the rest, since Windows sleeps overshoot.
void SleepUntilPrecise(std::chrono::steady_clock::time_point deadline, void* timer);

}  // namespace kameo::gpu
