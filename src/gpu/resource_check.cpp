#include "gpu/resource_check.h"

#include <cstdint>

#include <plume_vulkan.h>
#ifdef _WIN32
#include <plume_d3d12.h>

#include <dxgi1_4.h>
#endif

#include "gpu/graphics_system.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {

namespace {

bool UsingVulkan() {
  auto* gfx = GraphicsSystem::instance();
  return gfx && gfx->using_vulkan();
}

bool TextureBackingMissing(const plume::RenderTexture* texture) {
  if (UsingVulkan()) return static_cast<const plume::VulkanTexture*>(texture)->vk == VK_NULL_HANDLE;
#ifdef _WIN32
  return static_cast<const plume::D3D12Texture*>(texture)->d3d == nullptr;
#else
  return false;
#endif
}

bool BufferBackingMissing(const plume::RenderBuffer* buffer) {
  if (UsingVulkan()) return static_cast<const plume::VulkanBuffer*>(buffer)->vk == VK_NULL_HANDLE;
#ifdef _WIN32
  return static_cast<const plume::D3D12Buffer*>(buffer)->d3d == nullptr;
#else
  return false;
#endif
}

// With the first few failures only: a device that is out of memory or removed refuses everything after.
void LogDeviceState(plume::RenderDevice* device) {
  static int logged = 0;
  if (logged++ >= 8) return;
#ifdef _WIN32
  if (UsingVulkan()) return;
  auto* d3d12 = static_cast<plume::D3D12Device*>(device);
  if (d3d12->d3d != nullptr) {
    const HRESULT removed = d3d12->d3d->GetDeviceRemovedReason();
    if (FAILED(removed)) KGFX_ERROR("  the device is removed (0x{:08X})", uint32_t(removed));
  }
  IDXGIAdapter3* adapter = nullptr;
  if (d3d12->adapter != nullptr && SUCCEEDED(d3d12->adapter->QueryInterface(IID_PPV_ARGS(&adapter))) && adapter != nullptr) {
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
    DXGI_QUERY_VIDEO_MEMORY_INFO shared = {};
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
    KGFX_ERROR("  video memory: {} MB used of a {} MB budget on the card, {} MB used of {} MB shared", local.CurrentUsage >> 20, local.Budget >> 20, shared.CurrentUsage >> 20, shared.Budget >> 20);
    adapter->Release();
  }
#else
  (void)device;
#endif
}

}  // namespace

std::unique_ptr<plume::RenderTexture> CreateTextureChecked(plume::RenderDevice* device, const plume::RenderTextureDesc& desc, const char* what) {
  std::unique_ptr<plume::RenderTexture> texture = device->createTexture(desc);
  if (!texture || !TextureBackingMissing(texture.get())) return texture;
  KGFX_ERROR("{}: the device refused a {}x{}x{} texture of {} level(s), {} layer(s), format {}, {} sample(s), flags 0x{:X}; the error code is plume's line in logs/stderr.txt", what, desc.width, desc.height, desc.depth, desc.mipLevels, desc.arraySize, uint32_t(desc.format), desc.multisampling.sampleCount, uint32_t(desc.flags));
  LogDeviceState(device);
  return nullptr;
}

std::unique_ptr<plume::RenderFramebuffer> CreateFramebufferChecked(plume::RenderDevice* device, const plume::RenderFramebufferDesc& desc, const char* what) {
  std::unique_ptr<plume::RenderFramebuffer> framebuffer = device->createFramebuffer(desc);
  if (!framebuffer) {
    KGFX_ERROR("{}: framebuffer creation failed", what);
    return nullptr;
  }
#ifdef _WIN32
  // On D3D12 each attachment takes a descriptor from a fixed RTV or DSV heap, and a full heap leaves the framebuffer without one; clearing or drawing through it reads a null handle and crashes (issue #10).
  if (!UsingVulkan()) {
    const auto* d3d12 = static_cast<const plume::D3D12Framebuffer*>(framebuffer.get());
    const bool colors_missing = d3d12->colorHandles.size() != desc.colorAttachmentsCount;
    const bool depth_missing = desc.depthAttachment != nullptr && d3d12->depthTargetAllocatorOffset == plume::D3D12DescriptorHeapAllocator::INVALID_OFFSET;
    if (colors_missing || depth_missing) {
      static int logged = 0;
      if (logged++ < 8) KGFX_ERROR("{}: no free {} descriptor for the framebuffer; the draws aimed at it fall back to the scene", what, colors_missing ? "render target" : "depth stencil");
      return nullptr;
    }
  }
#endif
  return framebuffer;
}

std::unique_ptr<plume::RenderBuffer> CreateBufferChecked(plume::RenderDevice* device, const plume::RenderBufferDesc& desc, const char* what) {
  std::unique_ptr<plume::RenderBuffer> buffer = device->createBuffer(desc);
  if (!buffer || !BufferBackingMissing(buffer.get())) return buffer;
  KGFX_ERROR("{}: the device refused a {} byte buffer, heap type {}, flags 0x{:X}; the error code is plume's line in logs/stderr.txt", what, desc.size, int(desc.heapType), uint32_t(desc.flags));
  LogDeviceState(device);
  return nullptr;
}

}  // namespace kameo::gpu::detail
