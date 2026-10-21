#include "gpu/frame_profile.h"

#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>

#include "gpu/log.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_frame_profile, false, "Kameo", "Time each draw's texture, pipeline and upload work too, in the frame-time line the log prints every 300 frames. The per-frame stages and the GPU time are always there.");

namespace kameo::gpu::detail {

FrameProfile g_frameProfile;

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
  if (guest_frames > 0) line += fmt::format(" | game thread {:.1f} ms per frame, {:.1f} of it waiting for the renderer", double(guest_ns) * 1e-6 / double(guest_frames), double(stall_ns) * 1e-6 / double(guest_frames));
  line += fmt::format(" | {} draws per frame: vertices {:.1f}, constants {:.1f}", p.draws / p.frames, double(p.ns_vertices) * per_frame, double(p.ns_constants) * per_frame);
  if (FrameProfileDetailed()) line += fmt::format(", textures {:.1f}, pipelines {:.1f}, uploads {:.1f}", double(p.ns_textures) * per_frame, double(p.ns_pipelines) * per_frame, double(p.ns_uploads) * per_frame);
  KGFX_INFO("{}", line);
  p.ns_wait = p.ns_prep = p.ns_vertices = p.ns_constants = p.ns_textures = p.ns_pipelines = p.ns_uploads = p.ns_scene = p.ns_post = p.ns_present = p.ns_gpu = 0;
  p.draws = p.frames = p.gpu_frames = 0;
}

}  // namespace kameo::gpu::detail
