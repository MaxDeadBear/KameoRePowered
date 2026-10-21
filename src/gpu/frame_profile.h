#pragma once

// Per-stage frame timing, reported beside "presented frame" every 300 frames: where a slow scene spends its time on the game thread, the render thread and the GPU, readable from any machine's log.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace kameo::gpu::detail {

struct FrameProfile {
  // Written by the render thread only.
  uint64_t ns_wait = 0;       // the slot's fence, i.e. the GPU still busy with the frame before last
  uint64_t ns_prep = 0;       // shader modules, pipeline precache, draw-path resources
  uint64_t ns_vertices = 0;   // deferred vertex sources hashed and transcoded
  uint64_t ns_vertex_hash = 0;  // of which, the content hash that validates reuse (detailed only)
  uint64_t ns_constants = 0;  // constant records materialised
  uint64_t ns_textures = 0;   // EnsureTexture and samplers per draw (detailed only)
  uint64_t ns_pipelines = 0;  // EnsurePipeline per draw (detailed only)
  uint64_t ns_uploads = 0;    // geometry and constant uploads and binding per draw (detailed only)
  uint64_t ns_scene = 0;      // DrawCapturedScene as a whole
  uint64_t ns_post = 0;       // upscale, video, UI, gamma, close
  uint64_t ns_present = 0;    // the present call itself (vsync waits land here)
  uint64_t ns_gpu = 0;        // GPU time between the frame's first and last command
  uint32_t draws = 0;
  uint32_t frames = 0;
  uint32_t gpu_frames = 0;
  // Written by the game thread.
  std::atomic<uint64_t> ns_guest_frame{0};    // between consecutive frame handoffs
  std::atomic<uint64_t> ns_guest_stall{0};    // of which, waiting for the render thread
  std::atomic<uint64_t> ns_guest_capture{0};  // of which, inside the draw, resolve and clear hooks capturing the frame
  std::atomic<uint64_t> ns_guest_handoff{0};  // of which, publishing the captured frame
  std::atomic<uint32_t> guest_frames{0};
  // The capture's steps, detailed mode only.
  std::atomic<uint64_t> ns_cap_indices{0};    // reading, expanding and merge-checking the index list
  std::atomic<uint64_t> ns_cap_hash{0};       // of which, the content hash of the index bytes
  std::atomic<uint64_t> ns_cap_lookup{0};     // shader bindings, layout, topology
  std::atomic<uint64_t> ns_cap_vertices{0};   // vertex sources (deferred) or copies
  std::atomic<uint64_t> ns_cap_constants{0};  // both stages' constant records
  std::atomic<uint64_t> ns_cap_state{0};      // CaptureState
  std::atomic<uint64_t> ns_cap_textures{0};   // texture object addresses and read records
};

// Whether the capture hooks time their steps this frame; set by the frame handoff on the game thread, read there only.
extern bool g_captureDetailed;

// Adds the time it was alive to an atomic counter, for the game thread's hooks; a null counter measures nothing.
class ScopedAtomicNs {
 public:
  explicit ScopedAtomicNs(std::atomic<uint64_t>* counter) : counter_(counter), start_(counter ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
  ~ScopedAtomicNs() {
    if (counter_) counter_->fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count()), std::memory_order_relaxed);
  }
  ScopedAtomicNs(const ScopedAtomicNs&) = delete;
  ScopedAtomicNs& operator=(const ScopedAtomicNs&) = delete;

 private:
  std::atomic<uint64_t>* counter_;
  std::chrono::steady_clock::time_point start_;
};

extern FrameProfile g_frameProfile;

// Whether the per-draw timers run this frame (kameo_gfx_frame_profile); the per-frame stages and the GPU time are always measured. Read once per frame by DrawCapturedScene.
bool FrameProfileDetailed();

// Adds the time it was alive to `counter`; a null counter measures nothing.
class ScopedNs {
 public:
  explicit ScopedNs(uint64_t* counter) : counter_(counter), start_(counter ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
  ~ScopedNs() {
    if (counter_) *counter_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
  }
  ScopedNs(const ScopedNs&) = delete;
  ScopedNs& operator=(const ScopedNs&) = delete;

 private:
  uint64_t* counter_;
  std::chrono::steady_clock::time_point start_;
};

// Logs the averages since the last report and clears them.
void ReportFrameProfile();

}  // namespace kameo::gpu::detail
