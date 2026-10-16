#include "gpu/hooks/hooks_internal.h"

#include <chrono>
#include <cstring>
#include <string>

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/audio.h"
#include "gpu/config.h"
#include "gpu/draw_path.h"
#include "gpu/frame_pacing.h"
#include "gpu/log.h"
#include "gpu/movie/movie_replace.h"

using kameo::gpu::hooks::Active;

namespace {

// The fmv player fmvRender2D is drawing, so Draw_Bink_textures can find the movie it belongs to.
thread_local uint32_t t_renderingFmv = 0;

// An fmv player's movie name: the fmv file name without .bik, at +192 of its object at +8.
std::string FmvMovieName(uint8_t* base, uint32_t fmv) {
  std::string name;
  const uint32_t object = REX_LOAD_U32(fmv + 8);
  for (uint32_t i = 0; object && i < 64; ++i) {
    const char c = char(REX_LOAD_U8(object + 192 + i));
    if (!c) break;
    name.push_back(c);
  }
  return name;
}

// The Bink fmvRenderPlaying is holding under a replacement movie.
thread_local uint32_t t_heldBink = 0;

// Decoding and waiting on a Bink paced the frames around a movie to about 76 a second; held under a replacement nothing does, and screens that animate per frame (the title's pulsing Press Start) ran at hundreds. Frames are held to 60 a second instead, unless the frame rate limit is already lower.
void PaceReplacedMovie() {
  constexpr int32_t kMovieFps = 60;
  const int32_t cap = REXCVAR_GET(kameo_gfx_fps_cap);
  if (cap > 0 && cap <= kMovieFps) return;
  static std::chrono::steady_clock::time_point deadline;
  static void* timer = kameo::gpu::CreatePreciseTimer();
  const auto interval = std::chrono::nanoseconds(1000000000LL / kMovieFps);
  const auto now = std::chrono::steady_clock::now();
  if (deadline.time_since_epoch().count() == 0 || now - deadline > interval * 2) deadline = now;
  deadline += interval;
  const auto wait = deadline - std::chrono::steady_clock::now();
  if (wait <= std::chrono::milliseconds(1)) return;
  kameo::gpu::SleepUntilPrecise(deadline, timer);
}

}  // namespace

// BinkWait(bink) -- nonzero means the next frame is not due; a Bink held under a replacement is never due, so fmvRenderPlaying neither advances nor ends it.
extern "C" REX_FUNC(sub_826D1CE8) {
  if (t_heldBink && ctx.r3.u32 == t_heldBink) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_826D1CE8(ctx, base);
}

// fmvRenderPlaying(fmvPlayer) -- advances the Bink and ends the movie at its last frame; a replacement with its own sound runs the movie instead, so the Bink is silenced and held while the replacement's clock decides when it ends.
extern "C" REX_FUNC(sub_82264518) {
  const uint32_t fmv = ctx.r3.u32;
  const uint32_t bink = fmv ? REX_LOAD_U32(fmv + 12) : 0;
  if (!bink) {
    __imp__sub_82264518(ctx, base);
    return;
  }
  // The texture set at +16: its current frame-buffer index at +84, and that buffer's alpha plane texture at +32*index+12.
  const uint32_t index = REX_LOAD_U32(fmv + 16 + 84);
  const bool has_alpha = index <= 8 && REX_LOAD_U32(fmv + 16 + 32 * index + 12) != 0;
  const float gain = float(g_kameo_fmv_volume.load(std::memory_order_relaxed) * g_kameo_master_volume.load(std::memory_order_relaxed)) / 10000.0f;
  const kameo::gpu::MovieDrive drive = kameo::gpu::DriveReplacementMovie(FmvMovieName(base, fmv), bink, has_alpha, gain);
  if (drive == kameo::gpu::MovieDrive::Bink) {
    __imp__sub_82264518(ctx, base);
    return;
  }
  static uint32_t silenced = 0;
  if (silenced != bink) {
    silenced = bink;
    PPCContext call = ctx;
    call.r3.u64 = bink;
    call.r4.u64 = 0;
    sub_826D22D0(call, base);  // BinkSetSoundOnOff(bink, 0)
  }
  // The rest of the function still runs: volumes, the running time at +268, and the flags it sets once playback starts.
  const uint32_t outer_held = t_heldBink;
  t_heldBink = bink;
  __imp__sub_82264518(ctx, base);
  t_heldBink = outer_held;
  PaceReplacedMovie();
  if (drive != kameo::gpu::MovieDrive::Finished) return;
  // Looping (+216) starts the replacement over; otherwise the end the Bink's last frame would have signalled: status 6 through the render-thread buffer when the object has an ending (+248), else status 8.
  if (REX_LOAD_U32(fmv + 216)) {
    kameo::gpu::EndReplacementMovie(bink);
    return;
  }
  if (REX_LOAD_U32(fmv + 248)) {
    PPCContext call = ctx;
    call.r3.u64 = fmv;
    call.r4.u64 = 6;
    sub_82263338(call, base);
  } else {
    REX_STORE_U32(fmv + 208, 8);
  }
}

// BinkClose(bink) -- a skipped or finished movie's replacement stops with it.
extern "C" REX_FUNC(sub_826D19F8) {
  kameo::gpu::EndReplacementMovie(ctx.r3.u32);
  __imp__sub_826D19F8(ctx, base);
}

// Draw_Bink_textures(0x82265558) -- the Bink video blit.
// Everything is read from the struct in r4 (Hex-Rays mis-numbers this function's arguments across PPC's float/int split), with idx = struct[84]: plane pointers at +{92,104,116,128} + 48*idx, pitches at +{96,108,120,132} + 48*idx, luma size at +68/+72, chroma size at +76/+80.
extern "C" REX_FUNC(sub_82265558) {
  auto* gfx = Active();
  if (gfx == nullptr) {
    __imp__sub_82265558(ctx, base);
    return;
  }

  const uint32_t st = ctx.r4.u32;
  if (!st) {
    return;
  }

  kameo::gpu::BinkFrame frame;
  frame.luma_width = REX_LOAD_U32(st + 68);
  frame.luma_height = REX_LOAD_U32(st + 72);
  frame.chroma_width = REX_LOAD_U32(st + 76);
  frame.chroma_height = REX_LOAD_U32(st + 80);
  const uint32_t idx = REX_LOAD_U32(st + 84);

  // Implausible geometry means a wrong r4, which would otherwise be read as a huge texture and hang the upload.
  if (frame.luma_width == 0 || frame.luma_height == 0 || frame.luma_width > 4096 ||
      frame.luma_height > 4096 || idx > 8) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      KGFX_WARN("bink struct looks wrong (r4={:08X} {}x{} idx={}); skipping", st,
                  frame.luma_width, frame.luma_height, idx);
    }
    return;
  }

  // A plane exists only when its D3DTexture pointer at st + 32*idx + 4*i is set: Create_Bink_textures (0x822653F0) zeroes those every iteration, while the data pointers keep stale garbage.
  // The pixels start one 4 KB page after the stored pointer, D3D's own fetch-address adjustment for addresses at or above 0xE0000000.
  const uint32_t data_off[4] = {92, 104, 116, 128};
  const uint32_t pitch_off[4] = {96, 108, 120, 132};
  for (int i = 0; i < 4; ++i) {
    const uint32_t texture = REX_LOAD_U32(st + 32 * idx + 4 * i);
    const uint32_t ptr = texture ? REX_LOAD_U32(st + data_off[i] + 48 * idx) : 0;
    frame.pitches[i] = texture ? REX_LOAD_U32(st + pitch_off[i] + 48 * idx) : 0;
    // The page is the runtime's host offset for this view, which only exists where the view could not be mapped a page in (Windows); adding it by hand read a page late on Linux.
    frame.planes[i] = ptr ? REX_RAW_ADDR(ptr) : nullptr;
  }
  frame.has_alpha = frame.planes[3] != nullptr;

  // yuvtorgb: four float4 at 0x8273A5B0, big-endian in guest memory.
  for (int i = 0; i < 16; ++i) {
    const uint32_t bits = REX_LOAD_U32(0x8273A5B0 + 4 * i);
    std::memcpy(&frame.yuv_to_rgb[i], &bits, sizeof(float));
  }

  // Full-screen quad; the guest's letterbox offsets from x_g_kamVideoParams_1 are not applied.
  const float quad[4][5] = {
      {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
      {1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
      {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
      {1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
  };
  std::memcpy(frame.vertices, quad, sizeof(quad));
  frame.valid = true;


  // A replacement movie takes the picture; without its own sound it follows the Bink's frame (FrameNum at +12 counts from 1, FrameRate/FrameRateDiv at +20/+24).
  if (const uint32_t fmv = t_renderingFmv) {
    const uint32_t bink = REX_LOAD_U32(fmv + 12);
    const uint32_t rate = bink ? REX_LOAD_U32(bink + 20) : 0;
    const uint32_t rate_div = bink ? REX_LOAD_U32(bink + 24) : 0;
    if (rate && rate_div) {
      const uint32_t frame_number = REX_LOAD_U32(bink + 12);
      kameo::gpu::ReplaceMovieFrame(FmvMovieName(base, fmv), bink, double(frame_number ? frame_number - 1 : 0) * double(rate_div) / double(rate), &frame);
    }
  }

  gfx->SubmitBinkFrame(frame);
  kameo::gpu::CaptureBinkMarker(base);
}

// fmvRender2D(fmvPlayer) -- the player whose Bink Draw_Bink_textures is about to draw: +8 its object (movie name at +192), +12 the Bink handle.
extern "C" REX_FUNC(sub_822648A0) {
  const uint32_t outer = t_renderingFmv;
  t_renderingFmv = ctx.r3.u32;
  __imp__sub_822648A0(ctx, base);
  t_renderingFmv = outer;
}
