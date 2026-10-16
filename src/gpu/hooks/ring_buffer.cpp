#include "gpu/hooks/hooks_internal.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu/config.h"
#include "gpu/guest_device.h"
#include "gpu/log.h"

using kameo::gpu::hooks::Active;

namespace {

// The guest D3D ring is stubbed as re:Blue stubs it: nothing consumes it, so it hands out plain guest memory and never waits on a GPU.
struct GuestSink {
  uint32_t base = 0;
  uint32_t size = 0;
  std::atomic<uint32_t> used{0};
};
GuestSink g_commandSink;
GuestSink g_vertexSink;
constexpr uint32_t kCommandSinkBytes = 1u << 20;
constexpr uint32_t kVertexSinkBytes = 16u << 20;

}  // namespace

namespace kameo::gpu::hooks {

bool RingSinksReady() {
  static std::once_flag once;
  std::call_once(once, [] {
    rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeapByType(true, 4096);
    for (auto [sink, bytes] : {std::pair{&g_commandSink, kCommandSinkBytes}, std::pair{&g_vertexSink, kVertexSinkBytes}}) {
      uint32_t address = 0;
      if (heap && heap->Alloc(bytes, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) {
        sink->base = address;
        sink->size = bytes;
      }
    }
    if (g_commandSink.base && g_vertexSink.base) KGFX_INFO("guest D3D ring stubbed: commands to {:08X} ({} KB, never read), vertices to {:08X} ({} MB, per frame)", g_commandSink.base, kCommandSinkBytes >> 10, g_vertexSink.base, kVertexSinkBytes >> 20);
    else KGFX_ERROR("guest D3D ring: no physical memory for the sinks; D3D will report out of memory");
  });
  return g_commandSink.base && g_vertexSink.base;
}

uint32_t VertexSinkAlloc(uint32_t bytes) {
  const uint32_t rounded = (std::max(bytes, 16u) + 15u) & ~15u;
  uint32_t offset = g_vertexSink.used.fetch_add(rounded, std::memory_order_relaxed);
  if (offset + rounded > g_vertexSink.size) {
    static std::atomic<uint32_t> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 4) KGFX_WARN("guest vertex sink full ({} MB in one frame); wrapping", kVertexSinkBytes >> 20);
    g_vertexSink.used.store(rounded, std::memory_order_relaxed);
    offset = 0;
  }
  return g_vertexSink.base + offset;
}

void ResetVertexSink() { g_vertexSink.used.store(0, std::memory_order_relaxed); }

}  // namespace kameo::gpu::hooks

using kameo::gpu::hooks::RingSinksReady;
using kameo::gpu::hooks::VertexSinkAlloc;

// D3D::RingBufferDeviceAllocate(context, device, flags, size*, alignment), reached through device+13604: flag 1 asks for vertex data, otherwise for a command segment.
extern "C" REX_FUNC(sub_820CD188) {
  if (Active() == nullptr) {
    __imp__sub_820CD188(ctx, base);
    return;
  }
  if (!RingSinksReady()) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t size_at = ctx.r6.u32;
  if (ctx.r5.u32 & 1) {
    ctx.r3.u64 = VertexSinkAlloc(REX_LOAD_U32(size_at));
    return;
  }
  REX_STORE_U32(size_at, g_commandSink.size);
  ctx.r3.u64 = g_commandSink.base;
}

// D3D::CDevice::KickOff(CDevice*) -> command write pointer
// Inlined packet writers (xfix_sky_renderStars, Draw_Bink_textures, IconPlayerDrawBars, _FluidPoints_coreQuadRender) loop `if (write > limit) write = KickOff(device)`, so this rewinds into the command sink as StartNewSegment lays a segment out, and submits nothing.
extern "C" REX_FUNC(sub_820CDC88) {
  if (Active() == nullptr || !RingSinksReady()) {
    __imp__sub_820CDC88(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t write = g_commandSink.base - 4;
  const uint32_t end = g_commandSink.base + g_commandSink.size - 52;
  REX_STORE_U32(device + kameo::gpu::dev::kCmdWritePtr, write);
  REX_STORE_U32(device + kameo::gpu::dev::kCmdHardEndPtr, end);
  REX_STORE_U32(device + kameo::gpu::dev::kCmdEndPtr, end - 160);
  REX_STORE_U32(device + kameo::gpu::dev::kCmdSegmentStart, g_commandSink.base);
  REX_STORE_U32(device + kameo::gpu::dev::kCmdSegmentPredication, 0);
  REX_STORE_U32(device + kameo::gpu::dev::kCmdSegmentDataBytes, 0);
  ctx.r3.u64 = write;
}

// D3D::CDevice::BlockOnPrimaryRange(CDevice*, pos, size) -> wrapped end pos
// Only D3DDevice_SetRingBufferParameters still reaches it, and nothing reads the primary ring, so there is nothing to wait for.
extern "C" REX_FUNC(sub_820CD578) {
  if (Active() == nullptr) {
    __imp__sub_820CD578(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t end = ctx.r4.u32 + ctx.r5.u32;
  ctx.r3.u64 = device ? end & REX_LOAD_U32(device + kameo::gpu::dev::kPrimaryRingMask) : end;
}
