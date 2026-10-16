#include "gpu/hooks/hooks_internal.h"

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/log.h"
#include "ui/hud/fps_counter.h"

using kameo::gpu::hooks::Active;

// Every effect pass looks alike from the capture side (a quad through DrawVerticesUP), so each entry point below tags the draws it issues with its own address, which keeps the draw merger from joining draws of different passes.
#define KAMEO_PASS_HOOK(addr)                                   \
  extern "C" REX_FUNC(sub_##addr) {                             \
    const kameo::gpu::hooks::ScopedGuestPass pass(0x##addr);    \
    __imp__sub_##addr(ctx, base);                               \
  }

// The same, but the whole call is skipped while `skip` holds.
#define KAMEO_PASS_HOOK_UNLESS(addr, skip)                      \
  extern "C" REX_FUNC(sub_##addr) {                             \
    if (skip) return;                                           \
    const kameo::gpu::hooks::ScopedGuestPass pass(0x##addr);    \
    __imp__sub_##addr(ctx, base);                               \
  }

namespace {

// Set while _depthEffects_render runs with bloom switched off, so its plain bloom composite is skipped too.
thread_local bool t_skipBloomComposite = false;

}  // namespace

// _depthEffects_render(effects, view) -- the options clear its per-view effect bits (2 bloom, 8 heat haze) and depth-of-field strength for this call only.
extern "C" REX_FUNC(sub_8227C158) {
  const kameo::gpu::hooks::ScopedGuestPass pass(0x8227C158);
  const bool bloom = REXCVAR_GET(kameo_gfx_bloom);
  const bool depth_of_field = REXCVAR_GET(kameo_gfx_depth_of_field);
  const bool heat_haze = REXCVAR_GET(kameo_gfx_heat_haze);
  uint32_t flags_address = 0;
  uint32_t dof_address = 0;
  uint16_t flags = 0;
  uint32_t dof = 0;
  if (!bloom || !depth_of_field || !heat_haze) {
    // objectRenderThreadGetBufferData(0, tag, id), the same lookup the function makes first.
    PPCContext lookup = ctx;
    lookup.r3.u64 = 0;
    lookup.r4.u64 = 64592596;
    lookup.r5.u64 = REX_LOAD_U32(REX_LOAD_U32(ctx.r3.u32 + 48) + 76);
    sub_822562C0(lookup, base);
    const uint32_t buffer = lookup.r3.u32;
    const uint32_t view = REX_LOAD_U32(ctx.r4.u32 + 468);
    if (buffer) {
      flags_address = buffer + 704 + 2 * view;
      flags = REX_LOAD_U16(flags_address);
      uint16_t masked = flags;
      // The focus, shadow, water and film-grain composites and every cheat colour effect read the bloom texture as their input, so bloom is only removed under the plain bloom composite (index 0 at +192) with no cheat (dword_82B71AC8) active.
      if (!bloom && REX_LOAD_U8(buffer + 156 * view + 192) == 0 && REX_LOAD_U32(0x82B71AC8) == 0) {
        masked &= uint16_t(~2);
        t_skipBloomComposite = true;
      }
      if (!heat_haze) masked &= uint16_t(~8);
      REX_STORE_U16(flags_address, masked);
      if (!depth_of_field) {
        dof_address = buffer + 156 * view + 92;
        dof = REX_LOAD_U32(dof_address);
        REX_STORE_U32(dof_address, 0);
      }
    }
  }
  __imp__sub_8227C158(ctx, base);
  t_skipBloomComposite = false;
  if (flags_address) REX_STORE_U16(flags_address, flags);
  if (dof_address) REX_STORE_U32(dof_address, dof);
}
KAMEO_PASS_HOOK(8227D3E0)  // _depthEffects_RenderHeatHaze
KAMEO_PASS_HOOK(8227D638)  // _depthEffects_RenderDepthOfField
KAMEO_PASS_HOOK_UNLESS(8227D888, t_skipBloomComposite)  // _depthEffects_CompositeBloomEffect
KAMEO_PASS_HOOK(8227DB28)  // _depthEffects_CompositeFocusEffect
KAMEO_PASS_HOOK(8227E098)  // _depthEffects_CompositeShadowEffect
KAMEO_PASS_HOOK(8227E338)  // _depthEffects_CompositeSaturationEffect
KAMEO_PASS_HOOK(8227E3D8)  // _depthEffects_CompositeSolarisedEffect
KAMEO_PASS_HOOK(8227E458)  // xfix_depthEffects_bloomDownsampleAndComposite
KAMEO_PASS_HOOK(8227E720)  // _depthEffects_CompositeNegativeEffect
KAMEO_PASS_HOOK(8227E798)  // xfix_depthEffects_compositeBloomWithGrain
// _depthEffects_render reaches the bloom chain through these; the xfix_depthEffects downsample and blur passes have no code xref anywhere in the XEX, so hooking those reported zero calls.
KAMEO_PASS_HOOK(82280200)  // depthEffects_BlurTexture (the downsample AND every blur)
KAMEO_PASS_HOOK(82280438)  // depthEffects_ThresholdTexture (bloom threshold)
KAMEO_PASS_HOOK(822809C8)  // depthEffects_DesaturateTexture
KAMEO_PASS_HOOK(8227CBA0)  // _depthEffects_LuminanceHistogram
KAMEO_PASS_HOOK(8227CD10)  // xfix_depthEffects_applyGammaRamp
KAMEO_PASS_HOOK(8227D168)  // _depthEffects_RemapDepthBufferIntoColourBuffer
KAMEO_PASS_HOOK(8227F800)  // _depthEffects_CompositeWaterEffect
KAMEO_PASS_HOOK_UNLESS(822839F0, !REXCVAR_GET(kameo_gfx_heat_haze))  // xfix_effects_drawHeatHazeBillboards
KAMEO_PASS_HOOK(82284210)  // xfix_effects_drawScreenPerturbationQuad
KAMEO_PASS_HOOK(82282FC0)  // xfix_effects_drawNrmPrlxReflectQuads
KAMEO_PASS_HOOK(8230A128)  // xfix_effectsWater_drawCausticsQuad
KAMEO_PASS_HOOK(82308DF0)  // xfix_effectsBlobs_renderLightmap
KAMEO_PASS_HOOK(823550D8)  // xfix_pliteLightGlow_drawWithOcclusionQuery
KAMEO_PASS_HOOK(8213B618)  // skyClassRender
KAMEO_PASS_HOOK(8213C240)  // drawSun
KAMEO_PASS_HOOK(8213C720)  // xfixgem_sky_renderCloudClusters
KAMEO_PASS_HOOK(8213C998)  // xfix_sky_renderSunQuad
KAMEO_PASS_HOOK(8213CC38)  // xfix_sky_renderSunQuadFixedFunction
KAMEO_PASS_HOOK(8213DB58)  // xfix_sky_renderStars
KAMEO_PASS_HOOK_UNLESS(82112120, !REXCVAR_GET(kameo_gfx_particles))  // PLiteCoreRender
KAMEO_PASS_HOOK(824DAA60)  // pliteGrassRender
KAMEO_PASS_HOOK(824DC600)  // _grass_initRender
KAMEO_PASS_HOOK_UNLESS(824DCA18, !REXCVAR_GET(kameo_gfx_grass))  // _grass_coreRender

#undef KAMEO_PASS_HOOK
#undef KAMEO_PASS_HOOK_UNLESS

// modelRender(model, tag) -- skips the text box's shadow model where KameoSkipTextBoxShadow asks.
extern "C" REX_FUNC(sub_8224C028) {
  if (KameoSkipTextBoxShadow()) return;
  __imp__sub_8224C028(ctx, base);
}
