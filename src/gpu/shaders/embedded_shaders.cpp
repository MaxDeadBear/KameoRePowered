#include "gpu/shaders/embedded_shaders.h"

#include <set>
#include <utility>

#include "gpu/guest_state.h"
#include "gpu/log.h"
#include "gpu/shaders/particle_shaders.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/wet_lens_shaders.h"

namespace kameo::gpu::detail {

const plume::RenderSpecConstant kGuestSpecConstants[1] = {plume::RenderSpecConstant(0, (1u << 5) | (1u << 1))};

namespace {

std::unique_ptr<plume::RenderShader> EmbeddedShader(const GraphicsSystem::DrawPathContext& ctx, const unsigned char* dxil, size_t dxil_size, const unsigned char* spirv, size_t spirv_size) {
  if (ctx.spirv) return ctx.device->createShader(spirv, spirv_size, "main", plume::RenderShaderFormat::SPIRV);
  return ctx.device->createShader(dxil, dxil_size, "main", plume::RenderShaderFormat::DXIL);
}

#define KAMEO_EMBEDDED_SHADER(ctx, name) EmbeddedShader(ctx, shaders::name##Dxil, sizeof(shaders::name##Dxil), shaders::name##Spirv, sizeof(shaders::name##Spirv))

std::unique_ptr<plume::RenderShader> g_wetLensVS;

}  // namespace

PointSpriteVariant g_pointSprites[] = {
    {0x67EDA3D674722EC6ull, 0x9A16289A84E5A85Bull, "dust-mote", [](const GraphicsSystem::DrawPathContext& c) { return KAMEO_EMBEDDED_SHADER(c, g_moteVS); }, [](const GraphicsSystem::DrawPathContext& c) { return KAMEO_EMBEDDED_SHADER(c, g_motePS); }},
    {0x511A3E3E3F0761D3ull, 0xE1D5926C5F6332C6ull, "water-cannon spray", [](const GraphicsSystem::DrawPathContext& c) { return KAMEO_EMBEDDED_SHADER(c, g_sprayVS); }, [](const GraphicsSystem::DrawPathContext& c) { return KAMEO_EMBEDDED_SHADER(c, g_sprayPS); }},
    // The only other dumped vertex shader exporting a sprite size (r63.x): a vtxshaderfx dust-mote effect such as the burst when Kameo absorbs a baby warrior.
    {0x04D58836F1C8402Full, 0x84162BFF585D487Aull, "dust-mote burst", [](const GraphicsSystem::DrawPathContext& c) { return KAMEO_EMBEDDED_SHADER(c, g_burstVS); }, [](const GraphicsSystem::DrawPathContext& c) { return KAMEO_EMBEDDED_SHADER(c, g_burstPS); }},
};
std::unique_ptr<plume::RenderShader> g_pointSpriteGS;

PointSpriteVariant* FindPointSprite(const DrawCall& call) {
  if (!call.point_sprite || call.primitive != kPrimPointList || call.depth_only || !call.vs || !call.ps) return nullptr;
  for (PointSpriteVariant& v : g_pointSprites) {
    if (call.vs->hash == v.vs_hash && call.ps->hash == v.ps_hash) return &v;
  }
  // Without a variant the points draw one pixel wide, which reads as the effect missing.
  static std::set<std::pair<uint64_t, uint64_t>> reported;
  if (reported.size() < 32 && reported.insert({call.vs->hash, call.ps->hash}).second) KGFX_WARN("point-sprite draw without a native variant (drawn as single pixels): vs {:016X} ps {:016X}", call.vs->hash, call.ps->hash);
  return nullptr;
}

bool EnsurePointSpriteShaders(const GraphicsSystem::DrawPathContext& ctx, PointSpriteVariant& variant) {
  if (!g_pointSpriteGS) g_pointSpriteGS = KAMEO_EMBEDDED_SHADER(ctx, g_pointSpriteGS);
  if (!variant.vs) {
    variant.vs = variant.make_vs(ctx);
    variant.ps = variant.make_ps(ctx);
    if (variant.vs && variant.ps) KGFX_INFO("embedded {} point sprite shaders ready", variant.name);
  }
  return g_pointSpriteGS && variant.vs && variant.ps;
}

bool UsesWetLensVS(const DrawCall& call) {
  return call.vs && call.vs->hash == 0xB0B81D68886E069Dull;
}

plume::RenderShader* EnsureWetLensVS(const GraphicsSystem::DrawPathContext& ctx) {
  if (g_wetLensVS) return g_wetLensVS.get();
  g_wetLensVS = KAMEO_EMBEDDED_SHADER(ctx, g_wetLensVS);
  if (g_wetLensVS) KGFX_INFO("embedded wet-lens vertex shader ready");
  return g_wetLensVS.get();
}

void ReleaseEmbeddedShaders() {
  for (PointSpriteVariant& v : g_pointSprites) {
    v.vs.reset();
    v.ps.reset();
  }
  g_pointSpriteGS.reset();
  g_wetLensVS.reset();
}

}  // namespace kameo::gpu::detail
