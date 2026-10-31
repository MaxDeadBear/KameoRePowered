#pragma once

// Per-stage frame timing, reported beside "presented frame" every 300 frames: where a slow scene spends its time on the game thread, the render thread and the GPU, readable from any machine's log.

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>  // __rdtsc on clang and gcc; intrin.h is the MSVC spelling
#endif

namespace kameo::gpu::detail {

// The timers read the TSC rather than the performance counter: a sampled profile showed QueryPerformanceCounter and the timer objects at 40% of CaptureCommon's own time with per-draw timing on. Ticks are converted to nanoseconds at report time from a QPC calibration, so the per-draw cost is one rdtsc.
inline uint64_t ProfileTicks() {
#if defined(_MSC_VER) || defined(__x86_64__)
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
// Nanoseconds per tick, from the calibration ReportFrameProfile maintains; 0 until the first report window has passed.
double ProfileNsPerTick();

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
  // Whole-scene MSAA resolves recorded, and avoided by reusing unchanged colour.
  uint32_t msaa_resolves = 0;
  uint32_t msaa_resolve_reuses = 0;
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

// Adds the ticks it was alive to an atomic counter, for the game thread's hooks; a null counter measures nothing.
class ScopedAtomicNs {
 public:
  explicit ScopedAtomicNs(std::atomic<uint64_t>* counter) : counter_(counter), start_(counter ? ProfileTicks() : 0) {}
  ~ScopedAtomicNs() {
    if (counter_) counter_->fetch_add(ProfileTicks() - start_, std::memory_order_relaxed);
  }
  ScopedAtomicNs(const ScopedAtomicNs&) = delete;
  ScopedAtomicNs& operator=(const ScopedAtomicNs&) = delete;

 private:
  std::atomic<uint64_t>* counter_;
  uint64_t start_;
};

extern FrameProfile g_frameProfile;

// Whether the per-draw timers run this frame (kameo_gfx_frame_profile); the per-frame stages and the GPU time are always measured. Read once per frame by DrawCapturedScene.
bool FrameProfileDetailed();

// Adds the ticks it was alive to `counter`; a null counter measures nothing. Counters hold ticks; the report converts.
class ScopedNs {
 public:
  explicit ScopedNs(uint64_t* counter) : counter_(counter), start_(counter ? ProfileTicks() : 0) {}
  ~ScopedNs() {
    if (counter_) *counter_ += ProfileTicks() - start_;
  }
  ScopedNs(const ScopedNs&) = delete;
  ScopedNs& operator=(const ScopedNs&) = delete;

 private:
  uint64_t* counter_;
  uint64_t start_;
};

// Logs the averages since the last report and clears them.
void ReportFrameProfile();

}  // namespace kameo::gpu::detail
