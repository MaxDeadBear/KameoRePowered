#include "gpu/hooks/hooks_internal.h"

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu/config.h"
#include "gpu/log.h"

using kameo::gpu::hooks::Active;

namespace {

// The shadow map sizes xfix_shadow_createSurface writes, and the texture headers and depth surfaces it builds from them.
constexpr uint32_t kShadowScreens = 0x8273A5AC;
constexpr uint32_t kShadowLargeWidth = 0x82B7383C;
constexpr uint32_t kShadowLargeHeight = 0x82B73834;
constexpr uint32_t kShadowSmallWidth = 0x82B73830;
constexpr uint32_t kShadowSmallHeight = 0x82B73838;
constexpr uint32_t kShadowLargeTexture = 0x8286F160;
constexpr uint32_t kShadowSmallTexture = 0x8286F138;
constexpr uint32_t kShadowLargeSurface = 0x8286F134;
constexpr uint32_t kShadowSmallSurface = 0x8286F124;
constexpr uint32_t kShadowLargeMemory = 0x82B71964;
constexpr uint32_t kShadowSmallMemory = 0x82B71968;
constexpr uint32_t kD3DFmtD24S8 = 0x2D200196;

int32_t ShadowResolutionScale() {
  const int32_t scale = REXCVAR_GET(kameo_gfx_shadow_resolution);
  return scale >= 4 ? 4 : scale >= 2 ? 2 : 1;
}

// A texture header needs texel memory as large as its size, even though the maps are only ever written by resolves; one block per scale is kept.
// It comes from the runtime's physical heap, never the game's own pools: taking 25-100 MB out of those starved the next level load until an asset section failed to allocate.
uint32_t ShadowMemory(uint32_t size, int32_t scale, bool large) {
  static uint32_t blocks[2][5] = {};
  uint32_t& block = blocks[large ? 1 : 0][scale];
  if (!block) {
    if (rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeapByType(true, 4096)) {
      uint32_t address = 0;
      if (heap->Alloc(size * size * 4, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) block = address;
    }
    if (!block) KGFX_WARN("shadow map: no memory for a {}x{} map; using a smaller one", size, size);
  }
  return block;
}

void CreateShadowSurface(PPCContext& ctx, uint8_t* base, uint32_t size, uint32_t screens, uint32_t store_at) {
  static uint32_t parameters = 0;
  if (!parameters) parameters = rex::system::kernel_state()->memory()->SystemHeapAlloc(12);
  if (!parameters) return;
  uint32_t edram_base = 0;
  if (screens == 2) {
    PPCContext size_call = ctx;
    size_call.r3.u64 = REX_LOAD_U32(0x82759CDC);
    size_call.r4.u64 = REX_LOAD_U32(0x82759CE0);
    size_call.r5.u64 = REX_LOAD_U32(0x82759CC4);
    size_call.r6.u64 = REX_LOAD_U32(0x82759CD4);
    sub_820B11A0(size_call, base);
    edram_base = size_call.r3.u32;
  }
  REX_STORE_U32(parameters, edram_base);
  REX_STORE_U32(parameters + 4, 0);
  REX_STORE_U32(parameters + 8, 0);
  PPCContext call = ctx;
  call.r3.u64 = size;
  call.r4.u64 = size;
  call.r5.u64 = kD3DFmtD24S8;
  call.r6.u64 = 0;
  call.r7.u64 = parameters;
  sub_820C7210(call, base);
  REX_STORE_U32(store_at, call.r3.u32);
}

// With fur off the shell iterators run with NumFurShells at zero, the path the game takes for a furry model out of range.
void RunFurIterator(PPCContext& ctx, uint8_t* base, void (*iterator)(PPCContext&, uint8_t*)) {
  constexpr uint32_t kNumFurShells = 0x8270BA1F;
  const int32_t max_shells = REXCVAR_GET(kameo_gfx_fur) ? REXCVAR_GET(kameo_gfx_fur_max_shells) : 0;
  const uint8_t shells = REX_LOAD_U8(kNumFurShells);
  if (REXCVAR_GET(kameo_gfx_fur) && (max_shells <= 0 || shells <= uint8_t(max_shells))) {
    iterator(ctx, base);
    return;
  }
  REX_STORE_U8(kNumFurShells, REXCVAR_GET(kameo_gfx_fur) ? uint8_t(max_shells) : uint8_t(0));
  iterator(ctx, base);
  REX_STORE_U8(kNumFurShells, shells);
}

}  // namespace

// xfix_shadow_createSurface() -- the game builds 1024 and 768 maps; above scale 1 both are rebuilt at the larger size before any shadow pass uses them.
extern "C" REX_FUNC(sub_822604B0) {
  __imp__sub_822604B0(ctx, base);
  const int32_t scale = ShadowResolutionScale();
  if (scale == 1) return;
  const uint32_t screens = REX_LOAD_U32(kShadowScreens);
  struct ShadowMap {
    uint32_t base_size, width_at, height_at, texture, surface;
    bool large;
  };
  for (const ShadowMap& map : {ShadowMap{1024, kShadowLargeWidth, kShadowLargeHeight, kShadowLargeTexture, kShadowLargeSurface, true}, ShadowMap{768, kShadowSmallWidth, kShadowSmallHeight, kShadowSmallTexture, kShadowSmallSurface, false}}) {
    // A map whose memory cannot be found steps down a size; at scale 1 the game's own map stays.
    int32_t map_scale = scale;
    uint32_t memory = 0;
    while (map_scale > 1 && !(memory = ShadowMemory(map.base_size * map_scale, map_scale, map.large))) map_scale /= 2;
    if (map_scale == 1) continue;
    const uint32_t size = map.base_size * map_scale;
    REX_STORE_U32(map.width_at, size);
    REX_STORE_U32(map.height_at, size);
    PPCContext call = ctx;
    call.r3.u64 = map.texture;
    call.r4.u64 = size;
    call.r5.u64 = size;
    call.r6.u64 = memory;
    sub_82260430(call, base);
    CreateShadowSurface(ctx, base, size, screens, map.surface);
  }
}

// shadowRender(_, tag, view) -- a changed shadow resolution invalidates the screen count it rebuilds on, so the next pass recreates the maps.
extern "C" REX_FUNC(sub_822605D0) {
  static int32_t applied_scale = 0;
  const int32_t scale = ShadowResolutionScale();
  if (applied_scale != 0 && applied_scale != scale) REX_STORE_U32(kShadowScreens, 0xFFFFFFFF);
  applied_scale = scale;
  __imp__sub_822605D0(ctx, base);
}

// renderShadowVolumeRecord(light, record) -- one stencil shadow volume.
extern "C" REX_FUNC(sub_82262C00) {
  if (!REXCVAR_GET(kameo_gfx_shadows)) return;
  __imp__sub_82262C00(ctx, base);
}

// pliteSmoke_coreRender(smoke, _, _) -- smoke plume state setup and draw.
extern "C" REX_FUNC(sub_8234BEF0) {
  if (!REXCVAR_GET(kameo_gfx_particles)) return;
  __imp__sub_8234BEF0(ctx, base);
}

// _dustMote_coreRender(dustMote, _, _) -- the point-list field of floating motes.
extern "C" REX_FUNC(sub_8235BD60) {
  if (!REXCVAR_GET(kameo_gfx_dust_motes)) return;
  __imp__sub_8235BD60(ctx, base);
}

// sgFurIterator(node, group, stream)
extern "C" REX_FUNC(sub_820E56F8) { RunFurIterator(ctx, base, __imp__sub_820E56F8); }

// sgCommandStreamFurIterator(node, group, stream)
extern "C" REX_FUNC(sub_820E6C50) { RunFurIterator(ctx, base, __imp__sub_820E6C50); }
