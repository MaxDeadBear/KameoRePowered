#pragma once

#include <atomic>

namespace kameo::gpu {

// The scanout gamma ramp the guest installed through D3DDevice_SetGammaRamp, as the exponent of the pure power curve it builds; 1.0 until the game calls it.
// Written by the guest-thread hook and read by the present on the render thread.
extern std::atomic<float> g_guestGammaExponent;

}  // namespace kameo::gpu
