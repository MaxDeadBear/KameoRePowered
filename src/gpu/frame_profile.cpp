#include "gpu/frame_profile.h"

#include <chrono>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>

#include "core/fp_guard.h"
#include "gpu/log.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_frame_profile, false, "Kameo", "Time each draw's texture, pipeline and upload work too, in the frame-time line the log prints every 300 frames. The per-frame stages and the GPU time are always there.");

namespace kameo::gpu::detail {

FrameProfile g_frameProfile;
bool g_captureDetailed = false;

namespace {

// TSC ticks per nanosecond, measured against the steady clock: a short spin at the first report, then the whole run since it, which only gets more exact.
double g_nsPerTick = 0.0;
uint64_t g_baseline_ticks = 0;
std::chrono::steady_clock::time_point g_baseline_time;

void Calibrate() {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t ticks = ProfileTicks();
  if (g_nsPerTick == 0.0) {
    // About 2 ms once, on the render thread, before the first report.
    const auto until = now + std::chrono::milliseconds(2);
    auto later = now;
    while (later < until) later = std::chrono::steady_clock::now();
    const uint64_t later_ticks = ProfileTicks();
    if (later_ticks > ticks) g_nsPerTick = double(std::chrono::duration_cast<std::chrono::nanoseconds>(later - now).count()) / double(later_ticks - ticks);
    g_baseline_ticks = ticks;
    g_baseline_time = now;
    return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_baseline_time).count();
  if (elapsed > 1'000'000'000 && ticks > g_baseline_ticks) g_nsPerTick = double(elapsed) / double(ticks - g_baseline_ticks);
}

}  // namespace

double ProfileNsPerTick() { return g_nsPerTick; }

bool FrameProfileDetailed() { return REXCVAR_GET(kameo_gfx_frame_profile); }

void ReportFrameProfile() {
  FrameProfile& p = g_frameProfile;
  if (p.frames == 0) return;
  Calibrate();
  const double tick_ns = g_nsPerTick;
  const bool detailed = FrameProfileDetailed();
  // Tick totals to milliseconds per frame.
  const auto ms = [&](uint64_t ticks) { return double(ticks) * tick_ns * 1e-6 / double(p.frames); };

  const double render_total = ms(p.ns_wait + p.ns_prep + p.ns_scene + p.ns_post + p.ns_present);
  std::string line = fmt::format("frame times, average of {} frames: render thread {:.1f} ms (fence {:.1f}, shaders {:.1f}, scene {:.1f}, post {:.1f}, present {:.1f})", p.frames, render_total, ms(p.ns_wait), ms(p.ns_prep), ms(p.ns_scene), ms(p.ns_post), ms(p.ns_present));
  // GPU time arrives in nanoseconds from the query pool.
  if (p.gpu_frames > 0) line += fmt::format(" | gpu {:.1f} ms", double(p.ns_gpu) * 1e-6 / double(p.gpu_frames));

  const uint32_t guest_frames = p.guest_frames.exchange(0);
  const uint64_t guest_ticks = p.ns_guest_frame.exchange(0);
  const uint64_t stall_ticks = p.ns_guest_stall.exchange(0);
  const uint64_t capture_ticks = p.ns_guest_capture.exchange(0);
  const uint64_t handoff_ticks = p.ns_guest_handoff.exchange(0);
  const uint64_t cap_indices = p.ns_cap_indices.exchange(0);
  const uint64_t cap_hash = p.ns_cap_hash.exchange(0);
  const uint64_t cap_lookup = p.ns_cap_lookup.exchange(0);
  const uint64_t cap_vertices = p.ns_cap_vertices.exchange(0);
  const uint64_t cap_constants = p.ns_cap_constants.exchange(0);
  const uint64_t cap_state = p.ns_cap_state.exchange(0);
  const uint64_t cap_textures = p.ns_cap_textures.exchange(0);
  if (guest_frames > 0) {
    const auto gms = [&](uint64_t ticks) { return double(ticks) * tick_ns * 1e-6 / double(guest_frames); };
    line += fmt::format(" | game thread {:.1f} ms per frame: {:.1f} waiting for the renderer, {:.1f} capturing draws", gms(guest_ticks), gms(stall_ticks), gms(capture_ticks));
    if (detailed) line += fmt::format(" (indices {:.1f} of which hashing {:.1f}, lookups {:.1f}, vertices {:.1f}, constants {:.1f}, state {:.1f}, textures {:.1f})", gms(cap_indices), gms(cap_hash), gms(cap_lookup), gms(cap_vertices), gms(cap_constants), gms(cap_state), gms(cap_textures));
    const uint64_t own = guest_ticks > stall_ticks + capture_ticks + handoff_ticks ? guest_ticks - stall_ticks - capture_ticks - handoff_ticks : 0;
    line += fmt::format(", {:.1f} handing off, {:.1f} the game's own", gms(handoff_ticks), gms(own));
  }
  // Each masked FP exception the guest raises is a kernel trap on the game thread; a count in the thousands per window is a cost of its own.
  static uint64_t fp_exceptions_reported = 0;
  const uint64_t fp_exceptions = g_kameo_fp_exception_count.load(std::memory_order_relaxed);
  if (fp_exceptions != fp_exceptions_reported) {
    line += fmt::format(" | {} FP exceptions", fp_exceptions - fp_exceptions_reported);
    fp_exceptions_reported = fp_exceptions;
  }
  line += fmt::format(" | {} draws per frame: vertices {:.1f}", p.draws / p.frames, ms(p.ns_vertices));
  if (detailed) line += fmt::format(" of which hashing {:.1f}", ms(p.ns_vertex_hash));
  line += fmt::format(", constants {:.1f}", ms(p.ns_constants));
  if (detailed) line += fmt::format(", textures {:.1f}, pipelines {:.1f}, uploads {:.1f}", ms(p.ns_textures), ms(p.ns_pipelines), ms(p.ns_uploads));
  if (p.msaa_resolves || p.msaa_resolve_reuses) {
    line += fmt::format(" | MSAA resolves {:.1f}, reused {:.1f} per frame",
                        double(p.msaa_resolves) / p.frames, double(p.msaa_resolve_reuses) / p.frames);
  }
  KGFX_INFO("{}", line);
  p.ns_wait = p.ns_prep = p.ns_vertices = p.ns_vertex_hash = p.ns_constants = p.ns_textures = p.ns_pipelines = p.ns_uploads = p.ns_scene = p.ns_post = p.ns_present = p.ns_gpu = 0;
  p.draws = p.frames = p.gpu_frames = 0;
  p.msaa_resolves = p.msaa_resolve_reuses = 0;
}

}  // namespace kameo::gpu::detail
