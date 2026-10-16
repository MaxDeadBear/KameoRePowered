#pragma once

#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

// Reports a removed D3D12 device once and stays true afterwards; replay stops then, since every map would fail.
bool CheckDeviceRemoved(plume::RenderDevice* device);

}  // namespace kameo::gpu::detail
