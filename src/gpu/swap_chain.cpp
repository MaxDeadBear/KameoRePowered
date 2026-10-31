#include "gpu/graphics_system.h"

#include <plume_render_interface.h>
#include <plume_vulkan.h>

#ifdef _WIN32
#include <plume_d3d12.h>
#else
#include <SDL3/SDL.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"

namespace kameo::gpu {

bool WindowHasArea(void* window) {
#ifdef _WIN32
  RECT rect = {};
  if (window == nullptr || !GetClientRect(reinterpret_cast<HWND>(window), &rect)) {
    return false;
  }
  return (rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0;
#else
  int width = 0;
  int height = 0;
  return window != nullptr && SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window), &width, &height) && width > 0 && height > 0;
#endif
}

#ifdef _WIN32
IDXGISwapChain3* NativeSwapChain(plume::RenderSwapChain* swap_chain, bool using_vulkan) { return swap_chain && !using_vulkan ? static_cast<plume::D3D12SwapChain*>(swap_chain)->d3d : nullptr; }

bool BackBuffersStale(plume::RenderSwapChain* swap_chain, bool using_vulkan, void* window) {
  IDXGISwapChain3* native = NativeSwapChain(swap_chain, using_vulkan);
  RECT rect = {};
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  if (!native || !window || !GetClientRect(static_cast<HWND>(window), &rect) || FAILED(native->GetDesc1(&desc))) return false;
  return desc.Width != UINT(rect.right - rect.left) || desc.Height != UINT(rect.bottom - rect.top);
}
#endif

// DXGI forbids releasing a swap chain that is still in exclusive fullscreen, so every teardown comes through here first.
void GraphicsSystem::LeaveExclusiveFullscreen() {
#ifdef _WIN32
  IDXGISwapChain3* native = NativeSwapChain(swap_chain_.get(), using_vulkan_);
  BOOL fullscreen = FALSE;
  if (native && SUCCEEDED(native->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) {
    native->SetFullscreenState(FALSE, nullptr);
    KGFX_INFO("left exclusive fullscreen");
  }
#endif
  exclusive_last_state_ = false;
  exclusive_active_.store(false);
}

// ResizeBuffers needs every reference to the back buffers gone, so the frames in flight finish and the framebuffers and command lists built on them are dropped and rebuilt.
bool GraphicsSystem::ResizeSwapChainInPlace() {
  WaitForAllFrames();
  framebuffers_.clear();
  for (FrameSlot& slot : frames_) {
    slot.list.reset();
  }
  if (!swap_chain_->resize() || !CreateFrameResources()) {
    KGFX_WARN("resizing the swap chain after a fullscreen change failed; rebuilding it");
    LeaveExclusiveFullscreen();
    exclusive_wanted_.store(false);
    exclusive_last_state_ = false;
    framebuffers_.clear();
    for (FrameSlot& slot : frames_) {
      slot.list.reset();
    }
    swap_chain_.reset();
    swap_chain_window_ = nullptr;
    return false;
  }
  return true;
}

// Enters or leaves exclusive fullscreen to match the request; DXGI drops out on its own when the window loses focus, so entry is retried once it is the foreground window again.
void GraphicsSystem::UpdateExclusiveFullscreen() {
#ifdef _WIN32
  IDXGISwapChain3* native = NativeSwapChain(swap_chain_.get(), using_vulkan_);
  if (!native) return;
  BOOL fullscreen = FALSE;
  if (FAILED(native->GetFullscreenState(&fullscreen, nullptr))) return;
  // A flip-model swap chain rejects every Present after a fullscreen transition until its buffers are resized, including one DXGI makes itself on alt-tab.
  if ((fullscreen != FALSE) != exclusive_last_state_) {
    exclusive_last_state_ = fullscreen != FALSE;
    if (!ResizeSwapChainInPlace()) return;
  }
  const bool wanted = exclusive_wanted_.load();
  if (!wanted) {
    exclusive_entries_ = 0;
    if (fullscreen) {
      LeaveExclusiveFullscreen();
      exclusive_last_state_ = false;
      ResizeSwapChainInPlace();
    }
    exclusive_active_.store(false);
    return;
  }
  exclusive_active_.store(fullscreen != FALSE);
  if (fullscreen) {
    if (++exclusive_held_frames_ >= 300) exclusive_entries_ = 0;
    return;
  }
  exclusive_held_frames_ = 0;
  if (exclusive_retry_frames_ > 0) {
    --exclusive_retry_frames_;
    return;
  }
  if (::GetForegroundWindow() != static_cast<HWND>(swap_chain_window_)) return;
  // A mode change that never settles would rebuild the swap chain every frame, so repeated entries give up and stay borderless.
  if (exclusive_entries_ >= 5) return;
  ++exclusive_entries_;
  const HRESULT hr = native->SetFullscreenState(TRUE, nullptr);
  if (FAILED(hr)) {
    KGFX_WARN("exclusive fullscreen refused (0x{:08X}); staying borderless and retrying in a few seconds", uint32_t(hr));
    exclusive_retry_frames_ = 300;
    return;
  }
  exclusive_active_.store(true);
  exclusive_last_state_ = true;
  if (!ResizeSwapChainInPlace()) return;
  KGFX_INFO("entered exclusive fullscreen at {}x{}", swap_chain_->getWidth(), swap_chain_->getHeight());
  if (exclusive_entries_ >= 5) KGFX_WARN("exclusive fullscreen entered 5 times without holding; not retrying again this session");
#endif
}

bool GraphicsSystem::EnsureSwapChain() {
  if (swap_chain_) {
    return true;
  }
  if (swap_chain_failed_) {
    return false;  // do not retry every frame
  }

  void* native_window = get_native_window_ ? get_native_window_() : nullptr;
  if (!native_window) {
    return false;  // window not open yet; try again next frame
  }
  // plume sizes a Vulkan swap chain from the client rect and silently leaves it 0x0 when that is empty, which fails every texture built against it.
  if (!WindowHasArea(native_window)) {
    return false;  // try again next frame
  }

  plume::RenderSwapChainDesc swap_desc;
#ifdef _WIN32
  swap_desc.renderWindow = reinterpret_cast<plume::RenderWindow>(native_window);
#else
  // plume's Vulkan backend only builds Xlib surfaces, so a Wayland window has nothing to give it.
  const SDL_PropertiesID window_props = SDL_GetWindowProperties(static_cast<SDL_Window*>(native_window));
  swap_desc.renderWindow.display = static_cast<Display*>(SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  swap_desc.renderWindow.window = Window(SDL_GetNumberProperty(window_props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!swap_desc.renderWindow.display || !swap_desc.renderWindow.window) {
    KGFX_ERROR("the native renderer needs an X11 window and this one is not (SDL video driver \"{}\"); start the game with --video_driver=x11", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown");
    swap_chain_failed_ = true;
    return false;
  }
#endif
  swap_desc.format = plume::RenderFormat::B8G8R8A8_UNORM;
  swap_desc.textureCount = kSwapChainTextureCount;
  swap_chain_ = queue_->createSwapChain(swap_desc);
  swap_chain_window_ = native_window;
  // plume's Vulkan chain reports a resize while the present mode it was built with differs from the one asked for, and a new chain starts at FIFO, so asking only at present rebuilt it every frame with vsync off.
  if (swap_chain_) swap_chain_->setVsyncEnabled(REXCVAR_GET(kameo_gfx_vsync));
  // plume's Vulkan swap chain constructor never calls resize() and gives up when the window has no area, so it is sized here and retried next frame without latching swap_chain_failed_.
  if (swap_chain_ && (swap_chain_->getWidth() == 0 || swap_chain_->getHeight() == 0)) {
    swap_chain_->resize();
  }
  if (swap_chain_ && (swap_chain_->getWidth() == 0 || swap_chain_->getHeight() == 0)) {
    KGFX_WARN("swap chain still {}x{} after resize on a window that has area",
                swap_chain_->getWidth(), swap_chain_->getHeight());
    swap_chain_.reset();
    swap_chain_window_ = nullptr;
    return false;
  }
  if (!swap_chain_ || !CreateFrameResources()) {
    KGFX_ERROR("swap chain creation failed");
    swap_chain_.reset();
    swap_chain_failed_ = true;
    return false;
  }

  KGFX_INFO("swap chain ready ({}x{}, {} buffers)", swap_chain_->getWidth(),
              swap_chain_->getHeight(), swap_chain_->getTextureCount());
  return true;
}

void GraphicsSystem::WaitForAllFrames() {
  // plume's waitForCommandFence consumes an auto-reset event without checking a signal is outstanding, so waiting twice for one submission blocks forever; that froze the game on window resize.
  for (FrameSlot& slot : frames_) {
    if (!slot.pending) {
      continue;
    }
    slot.pending = false;
    queue_->waitForCommandFence(slot.fence.get());
  }
  // Fences do not cover presentation, which can still hold the swap chain and its semaphores.
  if (using_vulkan_ && queue_) {
    plume::VulkanQueue* vk_queue = static_cast<plume::VulkanCommandQueue*>(queue_.get())->queue;
    std::lock_guard<std::mutex> lock(*vk_queue->mutex);
    vkQueueWaitIdle(vk_queue->vk);
  }
}

plume::RenderCommandList* GraphicsSystem::CurrentCommandList() {
  return frames_[frame_slot_].list.get();
}

bool GraphicsSystem::CreateFrameResources() {
  // The fences are replaced below, so any signal the old ones were carrying is gone.
  frame_slot_ = 0;
  for (FrameSlot& slot : frames_) {
    slot.pending = false;
    slot.list = queue_->createCommandList();
    slot.fence = device_->createCommandFence();
    slot.acquire = device_->createCommandSemaphore();
    if (!slot.list || !slot.fence || !slot.acquire) {
      KGFX_ERROR("failed to create per-frame objects");
      return false;
    }
  }
  // One per swap chain image, not per frame slot: the presentation engine holds an image's semaphore until that image is acquired again.
  for (auto& semaphore : present_semaphores_) {
    semaphore = device_->createCommandSemaphore();
    if (!semaphore) {
      KGFX_ERROR("failed to create per-frame objects");
      return false;
    }
  }

  // One depth buffer shared by every swap chain image, standing in for the guest's EDRAM depth.
  // The gamma composition target shares this attachment even on a same-size rebuild.
  gamma_framebuffer_.reset();
  depth_view_.reset();
  // A zero-sized swap chain makes plume hand back a texture with a null image, which crashes createTextureView, so the invariant is checked rather than trusted.
  if (swap_chain_->getWidth() == 0 || swap_chain_->getHeight() == 0) {
    KGFX_ERROR("frame resources requested for a {}x{} swap chain",
                 swap_chain_->getWidth(), swap_chain_->getHeight());
    return false;
  }

  auto depth_desc = plume::RenderTextureDesc::DepthTarget(
      swap_chain_->getWidth(), swap_chain_->getHeight(), kDepthFormat);
  depth_texture_ = device_->createTexture(depth_desc);
  if (!depth_texture_) {
    KGFX_ERROR("depth target creation failed");
    return false;
  }
  depth_view_ = depth_texture_->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(kDepthFormat));

  framebuffers_.clear();
  for (uint32_t i = 0; i < swap_chain_->getTextureCount(); ++i) {
    const plume::RenderTexture* color = swap_chain_->getTexture(i);
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = depth_texture_.get();
    desc.depthAttachmentView = depth_view_.get();
    auto fb = device_->createFramebuffer(desc);
    if (!fb) {
      KGFX_ERROR("createFramebuffer failed for buffer {}", i);
      return false;
    }
    framebuffers_.push_back(std::move(fb));
  }
  return true;
}

}  // namespace kameo::gpu
