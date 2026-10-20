#pragma once

// Opt-in D3D12 diagnostics behind kameo_gfx_d3d12_debug, for device losses that only reproduce on someone else's GPU.

namespace plume {
struct RenderDevice;
}

namespace kameo::gpu::detail {

// Before the device exists: the debug layer (when the Graphics Tools feature is installed) and DRED breadcrumbs.
void EnableD3D12Diagnostics();

// After the device exists: opens the debug layer's message queue.
void AttachD3D12DebugMessages(plume::RenderDevice* device);

// Logs the debug layer's warnings and errors stored since the last call; called once a frame.
void DrainD3D12DebugMessages();

// On a removed device: the last GPU operations DRED saw complete and any faulting address.
void LogD3D12RemovalDetails(plume::RenderDevice* device);

}  // namespace kameo::gpu::detail
