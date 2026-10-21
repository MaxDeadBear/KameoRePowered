#include "gpu/hooks/hooks_internal.h"

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/draw_path.h"
#include "gpu/guest_device.h"
#include "gpu/log.h"

using kameo::gpu::hooks::Active;


namespace {

// Set around DrawVerticesUP's delegation, so the BeginVertices hook can tell its nested calls from standalone ones.
thread_local bool t_inDrawVerticesUP = false;

// xfixgem_drawTube_allocD3DRingVerticesIndices is an inlined BeginIndexedVertices whose only caller is drawTube (0x824FF560): it reserves ring memory and emits DRAW_INDX, and drawTube fills both rings after it returns.
struct PendingIndexedRing {
  uint32_t device = 0, primitive = 0, vertex_count = 0, index_count = 0, stride = 0, vertices = 0, indices = 0;
  uint32_t texture_objects[16] = {};
};
thread_local PendingIndexedRing t_pendingIndexedRing;

}  // namespace

#include "gpu/frame_profile.h"

// The hooks' own time on the game thread, so the frame-time line can split the game's work from the capture's.
#define KAMEO_TIME_CAPTURE() kameo::gpu::detail::ScopedAtomicNs kameo_capture_timed(&kameo::gpu::detail::g_frameProfile.ns_guest_capture)

// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, ...)
// Recorded in frame order, since the guest samples the destination texture later and without a resolve it stays black.
extern "C" REX_FUNC(sub_820C3460) {
  if (Active() != nullptr) {
    KAMEO_TIME_CAPTURE();
    kameo::gpu::CaptureResolve(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r10.u32, float(ctx.f1.f64), uint32_t(ctx.lr));
    // The original only builds resolve packets and, for a resolve that also clears, a nested ClearF; CaptureResolve records that clear from the flags.
    if (!REXCVAR_GET(kameo_gfx_guest_draw_packets)) return;
  }
  __imp__sub_820C3460(ctx, base);
}

// D3DDevice_ClearF(pDevice, Flags, pRect, pColor, Z, Stencil)
extern "C" REX_FUNC(sub_820C4F20) {
  // The colour half is what fills the sky wherever the level draws nothing.
  if (Active() != nullptr) {
    KAMEO_TIME_CAPTURE();
    if (ctx.r4.u32 & 0x1Fu) kameo::gpu::CaptureDepthClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64), uint32_t(ctx.lr));
    if (!REXCVAR_GET(kameo_gfx_guest_draw_packets)) return;
  }
  __imp__sub_820C4F20(ctx, base);
}

// D3DDevice_DrawVertices(pDevice, PrimitiveType, StartVertex, VertexCount) -- non-indexed geometry, captured like any other draw.
extern "C" REX_FUNC(sub_820CED48) {
  if (Active() != nullptr) {
    KAMEO_TIME_CAPTURE();
    kameo::gpu::CaptureDraw(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (!REXCVAR_GET(kameo_gfx_guest_draw_packets)) return;
  }
  __imp__sub_820CED48(ctx, base);
}

// D3DDevice_DrawIndexedVertices(pDevice, PrimitiveType, BaseVertexIndex, StartIndex, VertexCount) -- the main draw path; it takes no buffers and reads everything from device state.
extern "C" REX_FUNC(sub_820CEF88) {
  if (Active() != nullptr) {
    KAMEO_TIME_CAPTURE();
    // VertexCount is the number of indices, as in the PM4 packet the original builds; BaseVertexIndex is added to every index by the GPU.
    kameo::gpu::CaptureIndexedDraw(base, ctx.r3.u32, ctx.r4.u32, int32_t(ctx.r5.u32), ctx.r6.u32,
                                   ctx.r7.u32, uint32_t(ctx.lr), ctx.r31.u32, ctx.r17.u32);
    // The original only flushes pending shadow state into a command buffer nothing reads and appends a draw packet (processLazyVertexStreams, WriteLazy*, LazyWriteShaders).
    if (!REXCVAR_GET(kameo_gfx_guest_draw_packets)) return;
  }
  __imp__sub_820CEF88(ctx, base);
}

// D3DDevice_DrawVerticesUP(pDevice, PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride) -- everything the capture needs, the vertex pointer included, is in the arguments.
extern "C" REX_FUNC(sub_820CE9D8) {
  if (Active() != nullptr) {

    const uint32_t prim = ctx.r4.u32;
    const uint32_t count = ctx.r5.u32;
    const uint32_t data = ctx.r6.u32;
    const uint32_t stride = ctx.r7.u32;

    // Same shaders, declaration and state as any other draw; only the vertex data arrives as a pointer.
    KAMEO_TIME_CAPTURE();
    kameo::gpu::CaptureDrawUP(base, ctx.r3.u32, prim, count, data, stride);
  }
  // The original only copies the vertices into the ring through a nested BeginVertices, and CaptureDrawUP already has them.
  if (Active() != nullptr && !REXCVAR_GET(kameo_gfx_guest_draw_packets)) return;
  // Marks the nested BeginVertices so it can tell internal calls from standalone ones.
  t_inDrawVerticesUP = true;
  __imp__sub_820CE9D8(ctx, base);
  t_inDrawVerticesUP = false;
}

// D3DDevice_BeginVertices(pDevice, PrimitiveType, VertexCount, Stride) -- DrawVerticesUP calls it internally; the standalone calls are the 2D icon system and the Bink blit.
extern "C" REX_FUNC(sub_820CE738) {
  const bool standalone = !t_inDrawVerticesUP;
  // Read before delegating: r3 comes back holding the ring pointer.
  const uint32_t device = ctx.r3.u32;
  const uint32_t prim = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;

  if (Active() != nullptr) {
  }
  if (Active() != nullptr && !REXCVAR_GET(kameo_gfx_guest_draw_packets) && kameo::gpu::hooks::RingSinksReady()) {
    // The vertices go to the per-frame sink rather than a ring, and the inlined EndVertices restores the write pointer from kBeginVerticesResume, so it is left where it is.
    const uint32_t bytes = count * stride;
    const uint32_t vertices = kameo::gpu::hooks::VertexSinkAlloc(bytes);
    REX_STORE_U32(device + kameo::gpu::dev::kBeginVerticesResume, REX_LOAD_U32(device + kameo::gpu::dev::kCmdWritePtr));
    REX_STORE_U32(device + kameo::gpu::dev::kBeginVerticesData, vertices);
    REX_STORE_U32(device + kameo::gpu::dev::kBeginVerticesDwords, bytes >> 2);
    ctx.r3.u64 = vertices;
  } else {
    __imp__sub_820CE738(ctx, base);
  }

  // Standalone BeginVertices is the 2D icon system: health and energy bars and the warrior button prompts, which went missing while these were only counted. r3 now holds the ring the guest is about to write through.
  if (standalone && Active() != nullptr) {
    if (REXCVAR_GET(kameo_gfx_capture_begin_vertices)) {
      kameo::gpu::CaptureDrawBeginVertices(base, device, prim, count, ctx.r3.u32, stride);
    }
  }

  if (standalone && Active() != nullptr) {
  }
}

// xfixgem_drawTube_allocD3DRingVerticesIndices(device, prim, 0, vertex_count, index_count, 0, stride, &indices, ..., &vertices)
extern "C" REX_FUNC(sub_820CEA50) {
  PendingIndexedRing args;
  args.device = ctx.r3.u32;
  args.primitive = ctx.r4.u32;
  args.vertex_count = ctx.r6.u32;
  args.index_count = ctx.r7.u32;
  args.stride = ctx.r9.u32;
  // drawTube releases texture slots 0-3 (device + 0x31A0) before it returns, so the objects are read while still bound.
  for (uint32_t s = 0; s < 16; ++s) args.texture_objects[s] = REX_LOAD_U32(args.device + 0x31A0 + 4 * s);
  if (Active() != nullptr && !REXCVAR_GET(kameo_gfx_guest_draw_packets) && kameo::gpu::hooks::RingSinksReady()) {
    // Sized as the original sizes them; the vertex out-pointer is the ninth argument, on the caller's stack at +0x54.
    const uint32_t vertex_dwords = (8 * args.vertex_count) & 0x3FFFFFF8u;
    const uint32_t index_dwords = (args.index_count + 1) >> 1;
    const uint32_t vertices = kameo::gpu::hooks::VertexSinkAlloc(vertex_dwords * 4);
    const uint32_t indices = kameo::gpu::hooks::VertexSinkAlloc(index_dwords * 4);
    REX_STORE_U32(args.device + kameo::gpu::dev::kBeginVerticesResume, REX_LOAD_U32(args.device + kameo::gpu::dev::kCmdWritePtr));
    REX_STORE_U32(args.device + kameo::gpu::dev::kBeginVerticesData, vertices);
    REX_STORE_U32(args.device + kameo::gpu::dev::kBeginIndicesData, indices);
    REX_STORE_U32(args.device + kameo::gpu::dev::kBeginVerticesDwords, vertex_dwords);
    REX_STORE_U32(args.device + kameo::gpu::dev::kBeginIndicesDwords, index_dwords);
    REX_STORE_U32(ctx.r10.u32, indices);
    REX_STORE_U32(REX_LOAD_U32(ctx.r1.u32 + 0x54), vertices);
    ctx.r3.u64 = 0;
  } else {
    __imp__sub_820CEA50(ctx, base);
  }
  if (ctx.r3.u32 == 0) {
    t_pendingIndexedRing = args;
    t_pendingIndexedRing.vertices = REX_LOAD_U32(args.device + 13624);
    t_pendingIndexedRing.indices = REX_LOAD_U32(args.device + 13628);
  }
}

// drawTube() -- the water cannon's stream, captured once it returns, when both rings hold its geometry.
extern "C" REX_FUNC(sub_824FF560) {
  t_pendingIndexedRing = {};
  __imp__sub_824FF560(ctx, base);
  const PendingIndexedRing ring = t_pendingIndexedRing;
  t_pendingIndexedRing = {};
  if (ring.device && Active() != nullptr) {
    kameo::gpu::CaptureDrawIndexedUP(base, ring.device, ring.primitive, ring.vertex_count, ring.vertices, ring.stride, ring.index_count, ring.indices, ring.texture_objects);
  }
}
