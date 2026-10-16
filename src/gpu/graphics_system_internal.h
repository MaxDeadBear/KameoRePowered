#pragma once

// Shared by the files that define GraphicsSystem's members; nothing outside them includes this.

#include <chrono>
#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#ifdef _WIN32
#include <plume_d3d12.h>
#endif

namespace kameo::gpu {

// A frame this long is felt as a hitch rather than as a frame rate.
inline constexpr double kSpikeMs = 50.0;
inline constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
// Stencil is needed by the guest's volume passes, e.g. applyWetToScene's water spheres.
inline constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;

// A zero-area window is a minimise: rebuilding a swap chain against it fails, so the rebuild waits until the window has area again.
bool WindowHasArea(void* window);

#ifdef _WIN32
IDXGISwapChain3* NativeSwapChain(plume::RenderSwapChain* swap_chain, bool using_vulkan);
// Leaving exclusive fullscreen resizes the buffers while the window is still changing size, so DXGI can keep them at the old size; plume's needsResize compares the window with the size it recorded, not with the buffers, and never notices.
bool BackBuffersStale(plume::RenderSwapChain* swap_chain, bool using_vulkan, void* window);
#endif

inline uint64_t NanosSince(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

// One of the built-in shaders from gpu/shaders/builtin_shaders.h, in whichever form the backend consumes.
template <size_t DxilSize, size_t SpirvSize>
std::unique_ptr<plume::RenderShader> CreateBuiltinShader(plume::RenderDevice* device, plume::RenderShaderFormat format, const unsigned char (&dxil)[DxilSize], const unsigned char (&spirv)[SpirvSize], const char* entry) {
  if (format == plume::RenderShaderFormat::SPIRV) return device->createShader(spirv, SpirvSize, entry, format);
  return device->createShader(dxil, DxilSize, entry, format);
}

// A full-screen triangle drawn from SV_VertexID into the swap chain format with depth off; the depth format still has to match the swap chain framebuffers or Vulkan drops the draw.
std::unique_ptr<plume::RenderPipeline> CreateFullscreenPipeline(plume::RenderDevice* device, plume::RenderPipelineLayout* layout, plume::RenderShader* vs, plume::RenderShader* ps);

}  // namespace kameo::gpu
