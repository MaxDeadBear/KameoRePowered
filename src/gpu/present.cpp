#include "gpu/graphics_system.h"

#include <cstdlib>

#include <plume_render_interface.h>

#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/d3d12_debug.h"
#include "gpu/draw_path.h"
#include "gpu/frame_profile.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/occlusion.h"
#include "gpu/pipeline/pso_precache.h"

namespace kameo::gpu {

namespace {

// plume's end() discards Close()'s HRESULT, so a poisoned list removes the device at submit far from the bad call; closing here first surfaces it (the second Close is rejected harmlessly as message 547, and RenderDoc captures skip this because its replay dies on it).
bool CommandListClosesClean(plume::RenderCommandList* list, bool using_vulkan) {
#ifdef _WIN32
  static const bool under_renderdoc = GetModuleHandleW(L"renderdoc.dll") != nullptr;
  if (using_vulkan || list == nullptr || under_renderdoc) {
    return true;
  }
  auto* native = static_cast<plume::D3D12CommandList*>(list)->d3d;
  if (native == nullptr) {
    return true;
  }
  const HRESULT closed = native->Close();
  if (SUCCEEDED(closed)) {
    return true;
  }
  KGFX_ERROR("command list FAILED TO CLOSE (0x{:08X}). The frame is being DROPPED: "
               "submitting a list that failed to close is what removes the device, far from the "
               "call that poisoned it.",
               uint32_t(closed));
  return false;
#else
  (void)list;
  (void)using_vulkan;
  return true;
#endif
}

}  // namespace

// plume's resize() is not used: it faulted outright on a fullscreen toggle and cannot follow a replaced window handle, so a stale swap chain is torn down and rebuilt on the next frame.
bool GraphicsSystem::DropStaleSwapChain() {
  void* native_window = get_native_window_ ? get_native_window_() : nullptr;
#ifdef _WIN32
  const bool buffers_stale = swap_chain_ && !exclusive_last_state_ && native_window == swap_chain_window_ && BackBuffersStale(swap_chain_.get(), using_vulkan_, native_window);
#else
  const bool buffers_stale = false;
#endif
  const bool size_changed = swap_chain_ && swap_chain_->needsResize();
  if (swap_chain_ && WindowHasArea(native_window) &&
      (native_window != swap_chain_window_ || size_changed || buffers_stale)) {
    KGFX_INFO("rebuilding the swap chain ({})",
                native_window != swap_chain_window_ ? "window handle replaced" : size_changed ? "size changed" : "buffers kept an old size");
    WaitForAllFrames();
    LeaveExclusiveFullscreen();
    framebuffers_.clear();
    for (FrameSlot& slot : frames_) {
      slot.list.reset();
    }
    swap_chain_.reset();
    swap_chain_failed_ = false;  // a transient toggle must not latch the renderer off
    return true;
  }
  return false;
}

void GraphicsSystem::PresentClear() {
  if (DropStaleSwapChain()) {
    return;
  }

  if (!EnsureSwapChain() || swap_chain_->isEmpty()) {
    return;
  }
  UpdateExclusiveFullscreen();
  if (!swap_chain_) {
    return;
  }

  // A resize that bailed above leaves no command list to record into.
  if (!frames_[frame_slot_].list) {
    return;
  }

  // Wait for the last submission of the slot this frame reuses, which is kFramesInFlight frames old and has almost always retired, so recording overlaps the GPU.
  FrameSlot& frame = frames_[frame_slot_];
  const bool retired = frame.pending;
  if (frame.pending) {
    frame.pending = false;
    detail::ScopedNs timed(&detail::g_frameProfile.ns_wait);
    queue_->waitForCommandFence(frame.fence.get());
  }
  PublishOcclusionResults(frame_slot_, retired);
  // The slot's last frame has retired, so its two timestamps are the GPU time that frame took.
  if (frame.timestamps_written && frame.timestamps) {
    frame.timestamps_written = false;
    frame.timestamps->queryResults();
    const uint64_t* stamps = frame.timestamps->getResults();
    if (stamps[1] > stamps[0]) {
      detail::g_frameProfile.ns_gpu += stamps[1] - stamps[0];
      ++detail::g_frameProfile.gpu_frames;
    }
  }
  // Everything queued for destruction kFramesInFlight frames ago is provably idle now.
  RetireDrawPathGarbage(frame_index_);

  {
    detail::ScopedNs timed(&detail::g_frameProfile.ns_prep);
    // Shader modules are built here; the creation hooks run on guest threads and only queue them.
    QueuePipelinePrecacheShaders(*this);
    EnsureShaderModules();
    EnsureDrawPathResources();
    PrecacheDrawPipelines(*this);
    // Built here rather than on the first video: creating this pipeline costs a visible hitch, and the first bink plays during a load.
    EnsureBinkResources();
  }

  uint32_t index = 0;
  const bool acquired = swap_chain_->acquireTexture(frame.acquire.get(), &index);
  if (!acquired) {
    return;
  }

  plume::RenderTexture* texture = swap_chain_->getTexture(index);
  const uint32_t width = swap_chain_->getWidth();
  const uint32_t height = swap_chain_->getHeight();
  ui_width_.store(width, std::memory_order_relaxed);
  ui_height_.store(height, std::memory_order_relaxed);

  plume::RenderCommandList* list = frame.list.get();
  list->begin();
  if (!frame.timestamps) frame.timestamps = device_->createQueryPool(2);
  if (frame.timestamps) {
    list->resetQueryPool(frame.timestamps.get(), 0, 2);
    list->writeTimestamp(frame.timestamps.get(), 0);
  }
  uint32_t scene_width = width;
  uint32_t scene_height = height;
  SceneRenderSize(width, height, &scene_width, &scene_height);
  // A multisampled scene cannot be the swap chain image, so it always takes the scene target and is resolved onto the window.
  const uint32_t samples = SceneSampleCount();
  const bool scaled = (scene_width != width || scene_height != height || samples > 1) && EnsureUpscaleResources() && EnsureSceneTarget(scene_width, scene_height, samples);
  if (!scaled) {
    scene_width = width;
    scene_height = height;
  }
  plume::RenderTexture* scene_texture = scaled ? scene_color_.get() : texture;
  plume::RenderFramebuffer* scene_framebuffer = scaled ? scene_framebuffer_.get() : framebuffers_[index].get();
  active_scene_depth_ = scaled ? scene_depth_.get() : depth_texture_.get();
  active_scene_samples_ = scaled ? scene_samples_ : 1;
  // The shared depth buffer needs its own transition: clearing it in COMMON is rejected at submit.
  const plume::RenderTextureBarrier to_draw[] = {
      plume::RenderTextureBarrier(scene_texture, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(active_scene_depth_, plume::RenderTextureLayout::DEPTH_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_draw, uint32_t(std::size(to_draw)));
  list->setFramebuffer(scene_framebuffer);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(scene_width), float(scene_height)));
  list->setScissors(plume::RenderRect(0, 0, scene_width, scene_height));
  // Alpha clears to zero: the heat-haze shader reads backbuffer alpha as a perturbation mask, and most draws never write alpha.
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  list->clearDepthStencil(true, true, 1.0f, 0);
  // A new guest video frame can arrive during scene replay, so consuming pending data does not exclude the fallback.
  bink_in_scene_ = false;
  {
    detail::ScopedNs timed(&detail::g_frameProfile.ns_scene);
    DrawCapturedScene(*this, list, scene_texture, scene_framebuffer, scene_width,
                      scene_height);
  }
  detail::ScopedNs post_timed(&detail::g_frameProfile.ns_post);
  if (scaled) {
    // Everything after this -- video and UI -- belongs at the window's own resolution, so the scene is scaled onto the swap chain first.
    UpscaleScene(list, scene_texture, texture, framebuffers_[index].get(), width, height);
  }
  if (!bink_in_scene_) DrawBink(list, false);
  // Before ApplyGamma, which rebinds its own state, so the UI goes through the scanout ramp like everything else.
  DrawUi(list, framebuffers_[index].get(), width, height);
  // Last thing in the frame, exactly where the console's scanout ramp sits.
  ApplyGamma(list, texture, framebuffers_[index].get(), width, height);
  list->barriers(plume::RenderBarrierStage::NONE,
                          plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::PRESENT));
  if (frame.timestamps) {
    list->writeTimestamp(frame.timestamps.get(), 1);
    frame.timestamps_written = true;
  }
  // Before the close check, so the debug layer's account of a bad call lands in the log ahead of the close failure it causes.
  detail::DrainD3D12DebugMessages();
  if (!CommandListClosesClean(list, using_vulkan_)) {
    // A list that failed to close cannot be reset, so the next frame needs a new one.
    frame.list = queue_->createCommandList();
    frame.timestamps_written = false;  // the queries never ran
    return;
  }
  list->end();

  const plume::RenderCommandList* lists = list;
  plume::RenderCommandSemaphore* wait = frame.acquire.get();
  plume::RenderCommandSemaphore* signal = present_semaphores_[index].get();
  queue_->executeCommandLists(&lists, 1, &wait, 1, &signal, 1, frame.fence.get());
  frame.pending = true;

  swap_chain_->setVsyncEnabled(REXCVAR_GET(kameo_gfx_vsync));
#ifdef _WIN32
  // Exclusive fullscreen rejects DXGI_PRESENT_ALLOW_TEARING, which plume adds whenever vsync is off, so the flag is hidden from that present only.
  plume::D3D12SwapChain* native_chain = !using_vulkan_ ? static_cast<plume::D3D12SwapChain*>(swap_chain_.get()) : nullptr;
  const UINT chain_flags = native_chain ? native_chain->swapChainFlags : 0;
  if (native_chain && exclusive_active_.load()) native_chain->swapChainFlags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
#endif
  {
    detail::ScopedNs timed(&detail::g_frameProfile.ns_present);
    swap_chain_->present(index, &signal, 1);
  }
#ifdef _WIN32
  if (native_chain) native_chain->swapChainFlags = chain_flags;
#endif
  RecordPresentTime();
  LimitFrameRate();
  frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;

  ++detail::g_frameProfile.frames;
  if ((frame_index_++ % 300) == 0) {
    KGFX_INFO("presented frame {} ({}x{})", frame_index_, width, height);
    detail::ReportFrameProfile();
  }
}

}  // namespace kameo::gpu
