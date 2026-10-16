#include "gpu/hooks/hooks_internal.h"

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/log.h"
#include "gpu/occlusion.h"

using kameo::gpu::hooks::Active;

// The functions the guest uses to wait for its GPU. With no guest GPU they would wait forever, so they are replaced rather than satisfied; a vsync worker dispatching guest interrupts would mean rebuilding the emulator this replaces.

// D3DDevice_BlockUntilIdle(D3DDevice*) -> void
extern "C" REX_FUNC(sub_820CE0A0) {
  if (Active() == nullptr) {
    __imp__sub_820CE0A0(ctx, base);
    return;
  }
  Active()->WaitForRenderIdle("BlockUntilIdle");
}

// D3DDevice_IsFencePending(unsigned Fence) -> int
extern "C" REX_FUNC(sub_820CE128) {
  if (Active() == nullptr) {
    __imp__sub_820CE128(ctx, base);
    return;
  }
  // Pending only while the render thread is still recording the frame that fence belongs to.
  ctx.r3.u64 = Active()->IsGuestFencePending(ctx.r3.u32) ? 1 : 0;
}

constexpr uint32_t kQueryTypeOcclusion = 9;

// D3DQuery_Issue(D3DQuery*, DWORD Flags) -> void
extern "C" REX_FUNC(sub_820D07E0) {
  const uint32_t query = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  __imp__sub_820D07E0(ctx, base);
  if (Active() == nullptr || query == 0 || REX_LOAD_U32(query + 4) != kQueryTypeOcclusion) return;
  if (flags & 2) kameo::gpu::CaptureOcclusionQuery(base, query, true);
  else if (flags & 1) kameo::gpu::CaptureOcclusionQuery(base, query, false);
}

// D3DQuery_GetData(D3DQuery*, void* pData, DWORD Size, DWORD Flags) -> HRESULT
// The light glows scale by visible/total samples, so the count comes from our own GPU replaying the query's test quads; S_FALSE while it is in flight, as on hardware.
extern "C" REX_FUNC(sub_820D0968) {
  if (Active() == nullptr || ctx.r3.u32 == 0 || REX_LOAD_U32(ctx.r3.u32 + 4) != kQueryTypeOcclusion) {
    __imp__sub_820D0968(ctx, base);
    return;
  }
  uint32_t samples = 1;
  const bool ready = kameo::gpu::OcclusionQueryResult(ctx.r3.u32, &samples);
  if (ctx.r4.u32 != 0 && ctx.r5.u32 >= 4) REX_STORE_U32(ctx.r4.u32, ready ? samples : 1u);
  ctx.r3.u64 = ready ? 0 : 1;
}

// D3DDevice_SynchronizeToPresentationInterval(D3DDevice*) -> void
extern "C" REX_FUNC(sub_820CF868) {
  if (Active() == nullptr) {
    __imp__sub_820CF868(ctx, base);
    return;
  }
  // Pacing comes from the swap chain's own present, not from waiting on vblank.
}

// D3DResource_BlockUntilNotBusy(D3DResource*) -> void
extern "C" REX_FUNC(sub_820CE1B8) {
  if (Active() == nullptr) {
    __imp__sub_820CE1B8(ctx, base);
    return;
  }
  // A texture named by a frame still to be recorded is read from guest memory then; every read of one is keyed by its object.
  const uint32_t resource = kameo::gpu::CanonicalGuestAddress(ctx.r3.u32);
  Active()->WaitForRenderReads(resource, resource + 1, "BlockUntilNotBusy");
}

// D3D::CDevice::BlockOnFence(CDevice*, uint* target) -> void
// It spins until the GPU read-back word at **(device + 10384) catches up with the CPU fence at *(device + 10396), which nothing would ever write; publishing the fence there also resolves inlined fence comparisons that cannot be hooked.
extern "C" REX_FUNC(sub_820CDD80) {
  if (Active() == nullptr) {
    __imp__sub_820CDD80(ctx, base);
    return;
  }
  Active()->WaitForRenderIdle("BlockOnFence");

  const uint32_t device = ctx.r3.u32;
  if (device) {
    const uint32_t readback_ptr = REX_LOAD_U32(device + 10384);
    const uint32_t fence = REX_LOAD_U32(device + 10396);
    if (readback_ptr) {
      REX_STORE_U32(readback_ptr, fence);
    }
  }
}
