#include "gpu/device_removed.h"

#include <cstdint>

#include <plume_render_interface.h>
#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include "gpu/d3d12_debug.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {

bool CheckDeviceRemoved(plume::RenderDevice* device) {
#ifdef _WIN32
  static bool reported = false;
  if (reported) return true;
  // Only D3D12 can be asked; on Vulkan the device is a VulkanDevice and the cast below would crash.
  auto* gfx = GraphicsSystem::instance();
  if (!device || !gfx || gfx->using_vulkan()) return false;
  auto* d3d12_device = static_cast<plume::D3D12Device*>(device)->d3d;
  if (!d3d12_device) return false;
  const HRESULT reason = d3d12_device->GetDeviceRemovedReason();
  if (SUCCEEDED(reason)) return false;
  reported = true;
  KGFX_ERROR("D3D12 device removed, reason 0x{:08X}; scene replay stops", uint32_t(reason));
  LogD3D12RemovalDetails(device);
  return true;
#else
  (void)device;
  return false;
#endif
}

}  // namespace kameo::gpu::detail
