#include "gpu/graphics_system.h"

#include <plume_render_interface.h>
#include <plume_vulkan.h>

#ifndef _WIN32
#include <SDL3/SDL.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/ui/window.h>

#include "gpu/draw_path.h"
#include "gpu/frame_pacing.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"

namespace plume {
#ifdef _WIN32
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

// The X_STATUS_* macros cast to an unqualified `X_STATUS`, so they only compile where that name is visible.
using rex::X_STATUS;

namespace kameo::gpu {

GraphicsSystem* GraphicsSystem::s_instance = nullptr;

GraphicsSystem* GraphicsSystem::instance() { return s_instance; }

namespace {

const char* VendorName(plume::RenderDeviceVendor vendor) {
  switch (vendor) {
    case plume::RenderDeviceVendor::AMD: return "AMD";
    case plume::RenderDeviceVendor::NVIDIA: return "NVIDIA";
    case plume::RenderDeviceVendor::INTEL: return "Intel";
    default: return "unknown";
  }
}

}  // namespace

// The runtime only hands out a native handle on Windows; elsewhere the SDL window stands in for it, and this app opens exactly one.
void* NativeWindowHandle(rex::ui::Window* window) {
  if (!window) return nullptr;
#ifdef _WIN32
  return window->GetNativeWindowHandle();
#else
  static std::atomic<SDL_Window*> found{nullptr};
  if (SDL_Window* known = found.load()) return known;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* first = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  found.store(first);
  return first;
#endif
}

GraphicsSystem::GraphicsSystem(std::function<void*()> get_native_window, Backend backend)
    : get_native_window_(std::move(get_native_window)), backend_(backend) {}

GraphicsSystem::~GraphicsSystem() { Shutdown(); }

// D3D12 first on Windows, as in UnleashedRecomp and as the xenos reference creates, with a fallback to the other backend; both compile from the same plume sources.
bool GraphicsSystem::CreateInterface() {
  auto try_d3d12 = [&]() -> bool {
#ifdef _WIN32
    interface_ = plume::CreateD3D12Interface();
    if (interface_) {
      using_vulkan_ = false;
      return true;
    }
#endif
    return false;
  };

  auto try_vulkan = [&]() -> bool {
    interface_ = plume::CreateVulkanInterface();
    if (interface_) {
      using_vulkan_ = true;
      return true;
    }
    return false;
  };

  switch (backend_) {
    case Backend::D3D12:
      return try_d3d12();
    case Backend::Vulkan:
      return try_vulkan();
    case Backend::Auto:
    default:
#ifdef _WIN32
      return try_d3d12() || try_vulkan();
#else
      return try_vulkan();
#endif
  }
}

rex::X_STATUS GraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  (void)app_context;

  if (swap_chain_) {
    return X_STATUS_SUCCESS;  // idempotent, as the interface requires
  }

  if (!CreateInterface()) {
    KGFX_ERROR("no usable graphics backend");
    return X_STATUS_UNSUCCESSFUL;
  }

  device_ = interface_->createDevice();
  if (!device_) {
    KGFX_ERROR("createDevice failed ({})", using_vulkan_ ? "vulkan" : "d3d12");
    return X_STATUS_UNSUCCESSFUL;
  }

  const plume::RenderDeviceDescription& desc = device_->getDescription();
  KGFX_INFO("backend={} device=\"{}\" vendor={} vram={} MiB", using_vulkan_ ? "vulkan" : "d3d12", desc.name, VendorName(desc.vendor), desc.dedicatedVideoMemory / (1024 * 1024));

  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!queue_) {
    KGFX_ERROR("createCommandQueue failed");
    return X_STATUS_UNSUCCESSFUL;
  }

  // ReXApp::SetupPresentation calls this before it creates the window, so the swap chain waits for the first present.
  s_instance = this;
  KGFX_INFO("device ready; swap chain deferred until the window exists");
  return X_STATUS_SUCCESS;
}

// Deliberately empty: there is no vsync worker or interrupt callback, because the hooks replace the guest functions that would wait on them.
rex::X_STATUS GraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher, rex::system::KernelState* kernel_state) {
  (void)function_dispatcher;
  (void)kernel_state;
  KGFX_INFO("guest GPU not emulated (no ring buffer, no interrupts)");
  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  StopRenderThread();
  // Up to kFramesInFlight command lists can still be executing, and everything below releases resources they name.
  if (queue_) {
    WaitForAllFrames();
  }
  if (s_instance == this) {
    s_instance = nullptr;
  }
  ClosePreciseTimer(fps_cap_timer_);
  fps_cap_timer_ = nullptr;
  // Reverse construction order; every plume object's destructor goes through its device, so none may outlive device_.
  ReleaseDrawPathCaches();
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    ui_textures_.clear();
  }
  for (auto& buffer : ui_upload_) buffer.reset();
  for (auto& buffer : ui_vertices_) buffer.reset();
  ui_pipeline_.reset();
  ui_layout_.reset();
  ui_sampler_.reset();
  gamma_descriptors_.reset();
  gamma_pipeline_.reset();
  gamma_layout_.reset();
  gamma_constants_.reset();
  gamma_source_view_.reset();
  gamma_source_.reset();
  gamma_sampler_.reset();
  for (auto& descriptors : upscale_descriptors_) descriptors.reset();
  upscale_pipeline_.reset();
  upscale_layout_.reset();
  upscale_constants_.reset();
  upscale_sampler_.reset();
  scene_framebuffer_.reset();
  scene_color_view_.reset();
  scene_color_.reset();
  scene_depth_view_.reset();
  scene_depth_.reset();
  scene_resolved_view_.reset();
  scene_resolved_.reset();
  bink_descriptors_.reset();
  bink_pipelines_msaa_.clear();
  bink_pipeline_.reset();
  bink_layout_.reset();
  bink_ps_.reset();
  bink_vs_.reset();
  for (auto& view : bink_views_) view.reset();
  for (auto& texture : bink_textures_) texture.reset();
  for (auto& buffer : bink_upload_) buffer.reset();
  for (auto& buffer : bink_vertices_) buffer.reset();
  bink_constants_.reset();
  bink_sampler_.reset();
  draw_sampler_set_.reset();
  draw_cube_set_.reset();
  draw_texture3d_set_.reset();
  draw_texture_set_.reset();
  draw_layout_.reset();
  shader_modules_.clear();
  depth_view_.reset();
  depth_texture_.reset();
  framebuffers_.clear();
  for (auto& semaphore : present_semaphores_) semaphore.reset();
  for (FrameSlot& slot : frames_) {
    slot.acquire.reset();
    slot.fence.reset();
    slot.list.reset();
  }
  LeaveExclusiveFullscreen();
  swap_chain_.reset();
  queue_.reset();
  device_.reset();
  interface_.reset();
}

std::unique_ptr<plume::RenderPipeline> CreateFullscreenPipeline(plume::RenderDevice* device, plume::RenderPipelineLayout* layout, plume::RenderShader* vs, plume::RenderShader* ps) {
  plume::RenderGraphicsPipelineDesc pipeline_desc;
  pipeline_desc.pipelineLayout = layout;
  pipeline_desc.vertexShader = vs;
  pipeline_desc.pixelShader = ps;
  pipeline_desc.renderTargetFormat[0] = kColorFormat;
  pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  pipeline_desc.renderTargetCount = 1;
  pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  pipeline_desc.depthTargetFormat = kDepthFormat;
  pipeline_desc.depthEnabled = false;
  pipeline_desc.depthWriteEnabled = false;
  pipeline_desc.cullMode = plume::RenderCullMode::NONE;
  return device->createGraphicsPipeline(pipeline_desc);
}

}  // namespace kameo::gpu
