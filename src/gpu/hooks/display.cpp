#include "gpu/hooks/hooks_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/log.h"
#include "gpu/passes/gamma_pass.h"
#include "gpu/stereo/stereo.h"  // STEREO:

using kameo::gpu::hooks::Active;

// D3DDevice_SetGammaRamp(pDevice, Flags, const D3DGAMMARAMP* pRamp)
// The 360 applies the ramp at scanout; kamVideoGlobalGammaCorrection (0x820bf310) installs ramp[i] = pow((i+1)/256, 1.5 - bias) * 65535, and the gamma pass reapplies it at present.
extern "C" REX_FUNC(sub_820C2CF0) {
  const uint32_t ramp = ctx.r5.u32;
  __imp__sub_820C2CF0(ctx, base);
  if (!ramp) {
    return;
  }
  auto entry = [&](uint32_t i) { return uint32_t(REX_LOAD_U16(ramp + 2 * i)); };
  // The guest builds the ramp as a pure power curve, so ramp(0.5) = 0.5^e recovers the exponent, and the in-game gamma slider moves it too.
  const double mid = double(entry(127)) / 65535.0;
  const double e = (mid > 0.0 && mid < 1.0) ? std::log(mid) / std::log(0.5) : 1.0;
  const float clamped = float(std::min(3.0, std::max(0.25, e)));
  const float previous = kameo::gpu::g_guestGammaExponent.exchange(clamped);
  if (std::fabs(previous - clamped) > 0.001f) {
    KGFX_INFO("SetGammaRamp: [32]={} [64]={} [127]={} [191]={} [255]={} "
                "(identity 8191/16383/32767/49151/65535) -> exponent {:.3f}",
                entry(32), entry(64), entry(127), entry(191), entry(255), clamped);
  }
}

// mtx44Perspective(float* out, double fovDegrees, double aspect, double nearZ, double farZ)
// The aspect in f2 divides the matrix's X scale, so handing it the display's widens the view instead of squeezing the image; only the 16:9 scene projections are touched, not the shadow, decal and light passes.
extern "C" REX_FUNC(sub_820DFD38) {
  if (REXCVAR_GET(kameo_gfx_widescreen)) {
    if (auto* gfx = Active()) {
      const double aspect = ctx.f2.f64;
      const double display = gfx->DisplayAspect();
      if (display > aspect && aspect > 1.6 && aspect < 1.9) {
        ctx.f2.f64 = display;
      }
    }
  }
  __imp__sub_820DFD38(ctx, base);
}

// viewCalculateClipFrustumPerspective(view, out)
// The game culls against this frustum, whose half-width scales with view[1], the same aspect; it is widened for the call only, since the field is read for more than culling.
extern "C" REX_FUNC(sub_820ED110) {
  const uint32_t view = ctx.r3.u32;
  uint32_t saved = 0;
  bool patched = false;
  if (view) {
    saved = REX_LOAD_U32(view + 4);
    float aspect = 0.0f;
    std::memcpy(&aspect, &saved, sizeof(aspect));
    double widened = aspect;
    if (REXCVAR_GET(kameo_gfx_widescreen)) {
      if (auto* gfx = Active()) {
        const double display = gfx->DisplayAspect();
        if (display > aspect && aspect > 1.6f && aspect < 1.9f) widened = display;
      }
    }
    // STEREO: stereo shifts each eye's picture sideways by up to its separation (more for what is nearer than the convergence), so the frustum widens by that much again or the shift pulls in what the guest culled.
    if (kameo::gpu::StereoEyeCount() > 1) widened *= 1.0 + 2.0 * kameo::gpu::StereoSeparation();
    if (widened != aspect && aspect > 0.0f) {
      const float value = float(widened);
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      REX_STORE_U32(view + 4, bits);
      patched = true;
    }
  }
  __imp__sub_820ED110(ctx, base);
  if (patched) {
    REX_STORE_U32(view + 4, saved);
  }
}

// xfix_sky_renderCloudLayers() -- the sky gradient is a camera-space card at z -10 only 15.5 either side, so a wider field of view sees black past its edges; its colour changes only with height, so the card is widened after the guest fills the vertices it returns in r3, before the next device call reads them.
extern "C" REX_FUNC(sub_8213CE80) {
  constexpr uint32_t kFrameSize = 464;
  constexpr uint32_t kBandFlagsOffset = 0x60;
  constexpr uint32_t kVertexStride = 16;
  constexpr float kHalfWidth = 1000.0f;
  const uint32_t frame = ctx.r1.u32 - kFrameSize;
  {
    const kameo::gpu::hooks::ScopedGuestPass pass(0x8213CE80);
    __imp__sub_8213CE80(ctx, base);
  }
  const uint32_t vertices = ctx.r3.u32;
  if (!vertices) return;
  // var_170 bit 1 picks the twelve-vertex three-band card over the four-vertex one.
  const uint32_t count = (REX_LOAD_U8(frame + kBandFlagsOffset) & 2) != 0 ? 12 : 4;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t x = vertices + i * kVertexStride;
    const bool negative = (REX_LOAD_U32(x) & 0x80000000u) != 0;
    uint32_t bits;
    const float width = negative ? -kHalfWidth : kHalfWidth;
    std::memcpy(&bits, &width, sizeof(bits));
    REX_STORE_U32(x, bits);
  }
}

// xfixgem_renderThread_initVideoModeParams() -- the game only takes its 1280x720 branch for a HiDef mode at least 720 tall, and a smaller window reports one that is not, which leaves the native renderer a 640x480 standard-definition frame it draws black.
extern "C" REX_FUNC(sub_820BECA8) {
  __imp__sub_820BECA8(ctx, base);
  constexpr uint32_t kKamVideoParams = 0x8275D248;
  REX_STORE_U16(kKamVideoParams + 0x00, 1280);
  REX_STORE_U16(kKamVideoParams + 0x02, 720);
  REX_STORE_U8(kKamVideoParams + 0x04, 1);
  REX_STORE_U8(kKamVideoParams + 0x1F, 1);
  REX_STORE_U32(kKamVideoParams + 0x24, 0);
  REX_STORE_U32(0x82B70B34, 0);
}
