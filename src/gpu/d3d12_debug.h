#pragma once

// Opt-in D3D12 diagnostics behind kameo_gfx_d3d12_debug, for device losses that only reproduce on someone else's GPU.

namespace plume {
struct RenderDevice;
}

namespace kameo::gpu::detail {

// Before the device exists: the debug layer (when the Graphics Tools feature is installed) and DRED breadcrumbs.
void EnableD3D12Diagnostics();

// After the device exists: routes the debug layer's warnings and errors into the log as they happen.
void AttachD3D12DebugMessages(plume::RenderDevice* device);

// Logs what the message callback could not deliver on its own (Windows 10 has no callback, so this polls).
void DrainD3D12DebugMessages();

// On a removed device: the last GPU operations DRED saw complete and any faulting address.
void LogD3D12RemovalDetails(plume::RenderDevice* device);

}  // namespace kameo::gpu::detail
