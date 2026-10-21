#include "gpu/graphics_system.h"

#include <atomic>
#include <bit>

#include <plume_render_interface.h>
#include <plume_vulkan.h>

#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/draw_path.h"
#include "gpu/frame_profile.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/occlusion.h"

namespace kameo::gpu {

namespace {

// Raises a big-endian guest fence word to `fence`, never lowering it: the guest's BlockOnFence hook may already have written a newer value.
void AdvanceGuestFenceWord(uint32_t* word, uint32_t fence) {
  std::atomic_ref<uint32_t> ref(*word);
  uint32_t seen = ref.load(std::memory_order_relaxed);
  while (int32_t(fence - std::byteswap(seen)) > 0) {
    if (ref.compare_exchange_weak(seen, std::byteswap(fence), std::memory_order_release, std::memory_order_relaxed)) return;
  }
}

}  // namespace

void GraphicsSystem::WaitForRenderSlot() {
  std::unique_lock<std::mutex> lock(render_mutex_);
  if (!CapturedFrameWaiting()) return;
  const auto start = std::chrono::steady_clock::now();
  // Bounded so a render thread that stops taking frames costs a dropped frame rather than a hung game.
  render_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return render_stop_ || !CapturedFrameWaiting(); });
  detail::g_frameProfile.ns_guest_stall.fetch_add(NanosSince(start), std::memory_order_relaxed);
}

void GraphicsSystem::KickRenderThread(uint32_t guest_fence, uint32_t* readback_word, std::vector<uint32_t> guest_reads) {
  if (!REXCVAR_GET(kameo_gfx_render_thread)) {
    published_fence_.store(guest_fence, std::memory_order_release);
    PresentClear();
    completed_fence_.store(guest_fence, std::memory_order_release);
    if (readback_word) AdvanceGuestFenceWord(readback_word, guest_fence);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!render_thread_.joinable() && !render_stop_) render_thread_ = std::thread([this] { RenderThreadMain(); });
    render_fence_ = guest_fence;
    render_readback_ = readback_word;
    ++render_kicks_;
    while (!render_reads_.empty() && render_reads_.front().kick <= render_done_) render_reads_.pop_front();
    render_reads_.push_back({render_kicks_, std::move(guest_reads)});
  }
  published_fence_.store(guest_fence, std::memory_order_release);
  render_cv_.notify_all();
}

void GraphicsSystem::NotifyCapturedFrameTaken() {
  { std::lock_guard<std::mutex> lock(render_mutex_); }
  render_cv_.notify_all();
}

void GraphicsSystem::WaitForRenderIdle(const char* reason) {
  std::unique_lock<std::mutex> lock(render_mutex_);
  if (render_done_ == render_kicks_ || !render_thread_.joinable()) return;
  const auto start = std::chrono::steady_clock::now();
  render_cv_.wait_for(lock, std::chrono::seconds(2), [this] { return render_stop_ || render_done_ == render_kicks_; });
  const uint64_t waited = NanosSince(start);
  detail::g_frameProfile.ns_guest_stall.fetch_add(waited, std::memory_order_relaxed);
  // A single stall long enough to be seen as a hitch, which the 120-frame budget averages hide.
  if (waited > 8'000'000) {
    static uint32_t long_waits = 0;
    if (++long_waits < 64) KGFX_WARN("game thread stalled {:.1f} ms on the render thread ('{}')", double(waited) / 1e6, reason);
  }
}

void GraphicsSystem::WaitForRenderReads(uint32_t begin, uint32_t end, const char* reason) {
  std::unique_lock<std::mutex> lock(render_mutex_);
  if (!render_thread_.joinable()) return;
  while (!render_reads_.empty() && render_reads_.front().kick <= render_done_) render_reads_.pop_front();
  uint64_t last = 0;
  for (const PendingGuestReads& reads : render_reads_) {
    for (uint32_t address : reads.addresses) {
      if (address >= begin && address < end) {
        last = reads.kick;
        break;
      }
    }
  }
  if (last == 0) return;
  const auto start = std::chrono::steady_clock::now();
  render_cv_.wait_for(lock, std::chrono::seconds(2), [this, last] { return render_stop_ || render_done_ >= last; });
  const uint64_t waited = NanosSince(start);
  detail::g_frameProfile.ns_guest_stall.fetch_add(waited, std::memory_order_relaxed);
  if (waited > 8'000'000) {
    static uint32_t long_waits = 0;
    if (++long_waits < 64) KGFX_WARN("game thread stalled {:.1f} ms on the render thread ('{}', a frame in flight reads that memory)", double(waited) / 1e6, reason);
  }
}

bool GraphicsSystem::IsGuestFencePending(uint32_t fence) const {
  const uint32_t published = published_fence_.load(std::memory_order_acquire);
  const uint32_t completed = completed_fence_.load(std::memory_order_acquire);
  return int32_t(fence - published) <= 0 && int32_t(fence - completed) > 0;
}

void GraphicsSystem::RenderThreadMain() {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), kRenderThreadName);
#endif
  std::unique_lock<std::mutex> lock(render_mutex_);
  for (;;) {
    // The light glows re-issue a query only once the last one has answered, so a submitted frame's results are handed over as soon as its fence passes rather than when its slot is next recorded.
    while (!render_cv_.wait_for(lock, std::chrono::milliseconds(1), [this] { return render_stop_ || render_started_ != render_kicks_; })) PublishFinishedOcclusion();
    if (render_stop_) return;
    render_started_ = render_kicks_;
    const uint32_t fence = render_fence_;
    uint32_t* readback = render_readback_;
    lock.unlock();

    const uint64_t taken = CapturedFramesTaken();
    PresentClear();
    // A frame PresentClear could not take (a swap-chain rebuild, say) would otherwise hold the guest at its next handoff.
    if (CapturedFramesTaken() == taken) DiscardCapturedFrame();
    completed_fence_.store(fence, std::memory_order_release);
    if (readback) AdvanceGuestFenceWord(readback, fence);

    lock.lock();
    render_done_ = render_started_;
    render_cv_.notify_all();
  }
}

void GraphicsSystem::PublishFinishedOcclusion() {
  for (uint32_t s = 0; s < kFramesInFlight; ++s) {
    if (!frames_[s].pending || !frames_[s].fence) continue;
    if (using_vulkan_) {
      auto* fence = static_cast<plume::VulkanCommandFence*>(frames_[s].fence.get());
      if (vkGetFenceStatus(fence->device->vk, fence->vk) == VK_SUCCESS) PublishOcclusionResults(s, true);
      continue;
    }
#ifdef _WIN32
    auto* fence = static_cast<plume::D3D12CommandFence*>(frames_[s].fence.get());
    if (fence->d3d->GetCompletedValue() + 1 >= fence->fenceValue) PublishOcclusionResults(s, true);
#endif
  }
}

void GraphicsSystem::StopRenderThread() {
  {
    std::lock_guard<std::mutex> lock(render_mutex_);
    render_stop_ = true;
  }
  render_cv_.notify_all();
  if (render_thread_.joinable() && render_thread_.get_id() != std::this_thread::get_id()) render_thread_.join();
}

}  // namespace kameo::gpu
