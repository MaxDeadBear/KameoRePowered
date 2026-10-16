#pragma once

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

// The SPIR-V translations take the title-wide feature flags as a specialization constant where the DXIL ones have them baked in.
extern const plume::RenderSpecConstant kGuestSpecConstants[1];

// Point-sprite pairs with embedded variants that export the sprite size and take the sprite coordinate from point_sprite.hlsl.
using EmbeddedShaderFactory = std::unique_ptr<plume::RenderShader> (*)(const GraphicsSystem::DrawPathContext&);
struct PointSpriteVariant {
  uint64_t vs_hash;
  uint64_t ps_hash;
  const char* name;
  EmbeddedShaderFactory make_vs;
  EmbeddedShaderFactory make_ps;
  std::unique_ptr<plume::RenderShader> vs;
  std::unique_ptr<plume::RenderShader> ps;
};
extern std::unique_ptr<plume::RenderShader> g_pointSpriteGS;

PointSpriteVariant* FindPointSprite(const DrawCall& call);
bool EnsurePointSpriteShaders(const GraphicsSystem::DrawPathContext& ctx, PointSpriteVariant& variant);
// setWetSceneConstants draws with VIEWPORTENABLE off and derives the backbuffer UV from the position, which CaptureVertices has already moved into NDC.
bool UsesWetLensVS(const DrawCall& call);
plume::RenderShader* EnsureWetLensVS(const GraphicsSystem::DrawPathContext& ctx);
void ReleaseEmbeddedShaders();

}  // namespace kameo::gpu::detail
