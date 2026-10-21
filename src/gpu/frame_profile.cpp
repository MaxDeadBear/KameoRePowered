#include "gpu/frame_profile.h"

#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>

#include "core/fp_guard.h"
#include "gpu/log.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_frame_profile, false, "Kameo", "Time each draw's texture, pipeline and upload work too, in the frame-time line the log prints every 300 frames. The per-frame stages and the GPU time are always there.");

namespace kameo::gpu::detail {

FrameProfile g_frameProfile;
bool g_captureDetailed = false;

bool FrameProfileDetailed() { return REXCVAR_GET(kameo_gfx_frame_profile); }

void ReportFrameProfile() {
  FrameProfile& p = g_frameProfile;
  if (p.frames == 0) return;
  const double per_frame = 1e-6 / double(p.frames);  // ns totals to ms per frame
  const double render_total = double(p.ns_wait + p.ns_prep + p.ns_scene + p.ns_post + p.ns_present) * per_frame;
  std::string line = fmt::format("frame times, average of {} frames: render thread {:.1f} ms (fence {:.1f}, shaders {:.1f}, scene {:.1f}, post {:.1f}, present {:.1f})", p.frames, render_total, double(p.ns_wait) * per_frame, double(p.ns_prep) * per_frame, double(p.ns_scene) * per_frame, double(p.ns_post) * per_frame, double(p.ns_present) * per_frame);
  if (p.gpu_frames > 0) line += fmt::format(" | gpu {:.1f} ms", double(p.ns_gpu) * 1e-6 / double(p.gpu_frames));
  const uint32_t guest_frames = p.guest_frames.exchange(0);
  const uint64_t guest_ns = p.ns_guest_frame.exchange(0);
  const uint64_t stall_ns = p.ns_guest_stall.exchange(0);
  const uint64_t capture_ns = p.ns_guest_capture.exchange(0);
  const uint64_t handoff_ns = p.ns_guest_handoff.exchange(0);
  if (guest_frames > 0) {
    const double per_guest_frame = 1e-6 / double(guest_frames);
    line += fmt::format(" | game thread {:.1f} ms per frame: {:.1f} waiting for the renderer, {:.1f} capturing draws", double(guest_ns) * per_guest_frame, double(stall_ns) * per_guest_frame, double(capture_ns) * per_guest_frame);
    if (FrameProfileDetailed()) {
      line += fmt::format(" (indices {:.1f} of which hashing {:.1f}, lookups {:.1f}, vertices {:.1f}, constants {:.1f}, state {:.1f}, textures {:.1f})", double(p.ns_cap_indices.exchange(0)) * per_guest_frame, double(p.ns_cap_hash.exchange(0)) * per_guest_frame, double(p.ns_cap_lookup.exchange(0)) * per_guest_frame, double(p.ns_cap_vertices.exchange(0)) * per_guest_frame, double(p.ns_cap_constants.exchange(0)) * per_guest_frame, double(p.ns_cap_state.exchange(0)) * per_guest_frame, double(p.ns_cap_textures.exchange(0)) * per_guest_frame);
    }
    line += fmt::format(", {:.1f} handing off, {:.1f} the game's own", double(handoff_ns) * per_guest_frame, double(guest_ns > stall_ns + capture_ns + handoff_ns ? guest_ns - stall_ns - capture_ns - handoff_ns : 0) * per_guest_frame);
  }
  // Each masked FP exception the guest raises is a kernel trap on the game thread; a count in the thousands per window is a cost of its own.
  static uint64_t fp_exceptions_reported = 0;
  const uint64_t fp_exceptions = g_kameo_fp_exception_count.load(std::memory_order_relaxed);
  if (fp_exceptions != fp_exceptions_reported) {
    line += fmt::format(" | {} FP exceptions", fp_exceptions - fp_exceptions_reported);
    fp_exceptions_reported = fp_exceptions;
  }
  line += fmt::format(" | {} draws per frame: vertices {:.1f}", p.draws / p.frames, double(p.ns_vertices) * per_frame);
  if (FrameProfileDetailed()) line += fmt::format(" of which hashing {:.1f}", double(p.ns_vertex_hash) * per_frame);
  line += fmt::format(", constants {:.1f}", double(p.ns_constants) * per_frame);
  if (FrameProfileDetailed()) line += fmt::format(", textures {:.1f}, pipelines {:.1f}, uploads {:.1f}", double(p.ns_textures) * per_frame, double(p.ns_pipelines) * per_frame, double(p.ns_uploads) * per_frame);
  KGFX_INFO("{}", line);
  p.ns_wait = p.ns_prep = p.ns_vertices = p.ns_vertex_hash = p.ns_constants = p.ns_textures = p.ns_pipelines = p.ns_uploads = p.ns_scene = p.ns_post = p.ns_present = p.ns_gpu = 0;
  p.draws = p.frames = p.gpu_frames = 0;
}

}  // namespace kameo::gpu::detail
