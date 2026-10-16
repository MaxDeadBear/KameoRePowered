#include "gpu/hooks/hooks_internal.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/current_level.h"
#include "gpu/config.h"
#include "gpu/log.h"
#include "gpu/shaders/shader_registry.h"
#include "gpu/textures/texture_cache.h"
#include "gpu/textures/texture_pack.h"

using kameo::gpu::hooks::Active;

// D3DDevice_CreateVertexShader(const unsigned int *pFunction) -> D3DVertexShader*
extern "C" REX_FUNC(sub_820C86D0) {
  const uint32_t function = ctx.r3.u32;
  __imp__sub_820C86D0(ctx, base);
  if (Active() != nullptr) {
    kameo::gpu::RegisterShader(base, function, ctx.r3.u32, "vertex shader");
  }
}

// D3DDevice_CreatePixelShader(const unsigned int *pFunction) -> D3DPixelShader*
extern "C" REX_FUNC(sub_820C8360) {
  const uint32_t function = ctx.r3.u32;
  __imp__sub_820C8360(ctx, base);
  if (Active() != nullptr) {
    kameo::gpu::RegisterShader(base, function, ctx.r3.u32, "pixel shader");
  }
}

// texInitTexture(name, TextureAsset* asset) -- the database's "texture" load callback, which objectRenderThreadAssetInit always hands an empty name; asset+28 is the kind, asset+48 the texture object (for kind 4 an array of asset+52 of them).
extern "C" REX_FUNC(sub_820EBEF0) {
  const uint32_t asset = ctx.r4.u32;
  __imp__sub_820EBEF0(ctx, base);
  if (Active() == nullptr) return;
  if (asset) {
    const std::string level = KameoCurrentLevelName();
    const uint32_t objects = REX_LOAD_U32(asset + 48);
    const bool array = REX_LOAD_U32(asset + 28) == 4;
    const uint32_t count = array ? std::min<uint32_t>(REX_LOAD_U32(asset + 52), 1024) : 1;
    for (uint32_t i = 0; objects && i < count; ++i) {
      if (const uint32_t object = array ? REX_LOAD_U32(objects + 4 * i) : objects) kameo::gpu::NoteTextureAssetLevel(object, level);
    }
  }
  kameo::gpu::CaptureTextureInitialized(base, asset);
}

// D3D::DestroyResource(D3DResource*)
extern "C" REX_FUNC(sub_820D2B38) {
  if (Active() != nullptr) {
    kameo::gpu::CaptureTextureDestroyed(base, ctx.r3.u32);
  }
  __imp__sub_820D2B38(ctx, base);
}
