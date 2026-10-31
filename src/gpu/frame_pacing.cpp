#include "gpu/frame_pacing.h"

#include <algorithm>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "gpu/config.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu {

void* CreatePreciseTimer() {
#ifdef _WIN32
  return ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#else
  return nullptr;
#endif
}

void ClosePreciseTimer(void* timer) {
#ifdef _WIN32
  if (timer) ::CloseHandle(static_cast<HANDLE>(timer));
#else
  (void)timer;
#endif
}

// A minimized Win32 window may still report its old client size and present
// without waiting. Slow hidden frames to at most 15 FPS, but replay each one:
// one-time offscreen and shadow renders must still populate their textures.
void GraphicsSystem::PaceMinimizedWindow() {
#ifdef _WIN32
  const auto window = static_cast<HWND>(get_native_window_ ? get_native_window_() : nullptr);
  const bool minimized = window != nullptr && ::IsIconic(window);
  if (minimized != minimized_last_) {
    minimized_last_ = minimized;
    fps_cap_deadline_ = {};
    present_times_.clear();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    frame_stats_ = {};
  }
  // Use a blocking wait, with no spin, and check the window again next frame
  // so restoring it costs no more than this one 67 ms wait.
  if (minimized) std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / 15));
#endif
}

void SleepUntilPrecise(std::chrono::steady_clock::time_point deadline, void* timer) {
  const auto spin_margin = std::chrono::milliseconds(1);
  const auto now = std::chrono::steady_clock::now();
  if (deadline - now > spin_margin) {
    const auto sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now - spin_margin);
#ifdef _WIN32
    if (timer) {
      LARGE_INTEGER due{};
      due.QuadPart = -std::max<LONGLONG>(1, sleep.count() / 100);
      if (::SetWaitableTimer(static_cast<HANDLE>(timer), &due, 0, nullptr, nullptr, FALSE)) ::WaitForSingleObject(static_cast<HANDLE>(timer), INFINITE);
    } else {
      std::this_thread::sleep_for(sleep);
    }
#else
    (void)timer;
    std::this_thread::sleep_for(sleep);
#endif
  }
  while (std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
}

// Holds each present to the frame rate cap's interval.
void GraphicsSystem::LimitFrameRate() {
  const int32_t cap = REXCVAR_GET(kameo_gfx_fps_cap);
  if (cap <= 0) {
    fps_cap_deadline_ = {};
    return;
  }
  const auto interval = std::chrono::nanoseconds(1000000000LL / std::clamp(cap, 10, 1000));
  const auto now = std::chrono::steady_clock::now();
  // A frame that ran long starts a fresh schedule rather than racing to catch up.
  if (fps_cap_deadline_.time_since_epoch().count() == 0 || now - fps_cap_deadline_ > interval * 2) fps_cap_deadline_ = now;
  fps_cap_deadline_ += interval;
  if (!fps_cap_timer_) fps_cap_timer_ = CreatePreciseTimer();
  SleepUntilPrecise(fps_cap_deadline_, fps_cap_timer_);
}

void GraphicsSystem::RecordPresentTime() {
  const auto now = std::chrono::steady_clock::now();
  present_times_.push_back(now);
  while (present_times_.size() > 2 && now - present_times_.front() > std::chrono::seconds(1)) present_times_.pop_front();
  if (present_times_.size() < 2) return;
  double worst = 0.0;
  for (size_t i = 1; i < present_times_.size(); ++i) worst = std::max(worst, std::chrono::duration<double, std::milli>(present_times_[i] - present_times_[i - 1]).count());
  const double span = std::chrono::duration<double, std::milli>(present_times_.back() - present_times_.front()).count();
  const double frames = double(present_times_.size() - 1);
  std::lock_guard<std::mutex> lock(stats_mutex_);
  frame_stats_ = FrameStats{span > 0.0 ? frames * 1000.0 / span : 0.0, span / frames, worst};
}

GraphicsSystem::FrameStats GraphicsSystem::GetFrameStats() {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return frame_stats_;
}

}  // namespace kameo::gpu
