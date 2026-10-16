#include "gpu/draw/draw_merge.h"

#include <cstring>

#include "gpu/config.h"
#include "gpu/constant_buffers.h"
#include "gpu/draw/capture.h"
#include "gpu/draw/render_state.h"
#include "gpu/draw/vertex_capture.h"
#include "gpu/guest_memory.h"

namespace kameo::gpu::detail {

std::vector<uint32_t> g_pendingDeferred;
std::atomic<uint64_t> g_mergedDraws{0};
std::atomic<uint64_t> g_mergedIndexedDraws{0};

namespace {

// A pending BeginVertices draw that later identical BeginVertices calls were folded into: its finished ring segments, and the vertex count of the open one.
constexpr uint32_t kNoMerge = ~0u;
constexpr uint32_t kMaxMergedVertices = 1u << 16;
uint32_t g_mergeDraw = kNoMerge;
uint32_t g_mergeOpenCount = 0;
std::vector<uint8_t> g_mergeGather;
// The same for indexed draws: their expanded index lists concatenated here and written to the arena when the run ends.
constexpr uint32_t kMaxMergedIndices = 1u << 20;
uint32_t g_indexMergeDraw = kNoMerge;
std::vector<uint32_t> g_indexMergeList;

// True when the device would capture this draw with the target's shaders, declaration, constant blocks, state and texture addresses.
bool MatchesTarget(uint8_t* base, uint32_t device, const DrawCall& target) {
  if (Load32(base, device + kDeviceVertexShader) != target.vs_object || Load32(base, device + kDevicePixelShader) != target.ps_object || Load32(base, device + kDeviceVertexDeclaration) != target.guest_decl) return false;
  if (g_vsDedup.offset != target.vs_constants || g_psDedup.offset != target.ps_constants) return false;
  if (!ConstantsUnchanged(base, device, g_vsDedup, SpanOf(ShaderConstantRangeOf(target.vs))) || !ConstantsUnchanged(base, device, g_psDedup, SpanOf(ShaderConstantRangeOf(target.ps)))) return false;
  static thread_local DrawCall probe;
  CaptureState(base, device, &probe, false);
  if (!SameCapturedState(probe, target)) return false;
  for (uint32_t s = 0; s < kSamplerCount; ++s) {
    const uint32_t address = probe.texture_objects[s] ? (Load32(base, probe.texture_objects[s] + 20) & 0xFFFFF000u) : 0;
    if (address != target.texture_addresses[s]) return false;
  }
  return true;
}

}  // namespace

void ResolveDeferredVertices(uint8_t* base) {
  if (g_pendingDeferred.empty() || base == nullptr) {
    return;
  }
  for (uint32_t index : g_pendingDeferred) {
    if (index >= g_capturing.draws.size()) {
      continue;
    }
    DrawCall& call = g_capturing.draws[index];
    if (!call.deferred_vertex_addr || !call.layout || !call.deferred_vertex_count) {
      continue;
    }
    StreamOverride ring;
    const bool merged = index == g_mergeDraw && !g_mergeGather.empty();
    if (merged) {
      const uint8_t* open = GuestData(base, call.deferred_vertex_addr);
      g_mergeGather.insert(g_mergeGather.end(), open, open + size_t(g_mergeOpenCount) * call.deferred_vertex_stride);
      ring.data = g_mergeGather.data();
    } else {
      ring.data = GuestData(base, call.deferred_vertex_addr);
    }
    ring.stride = call.deferred_vertex_stride;
    ring.count = call.deferred_vertex_count;
    uint32_t index_base = 0;
    uint32_t count = 0;
    if (!CaptureVertices(base, call.deferred_device, *call.layout, 0,
                         call.deferred_vertex_count - 1, &g_capturing, &call.vertex_data,
                         &call.vertex_bytes, &index_base, &count, &call.vertex_blob,
                         call.viewport, ring)) {
      call.vertex_data = kNoData;
      call.vertex_blob.reset();
    }
    if (merged) {
      static thread_local std::vector<uint32_t> merged_indices;
      merged_indices.clear();
      uint32_t lo = 0;
      uint32_t hi = 0;
      if (ExpandTopology(call.primitive, call.deferred_vertex_count, nullptr, &merged_indices, &lo, &hi) && !merged_indices.empty()) {
        call.index_count = uint32_t(merged_indices.size());
        call.index_data = g_capturing.alloc(call.index_count * 4, 4);
        call.index_blob.reset();
        std::memcpy(g_capturing.at(call.index_data), merged_indices.data(), size_t(call.index_count) * 4);
        call.vertex_count = call.deferred_vertex_count;
      } else {
        call.vertex_data = kNoData;
        call.vertex_blob.reset();
      }
    }
    call.deferred_vertex_addr = 0;
  }
  g_pendingDeferred.clear();
  g_mergeDraw = kNoMerge;
  g_mergeGather.clear();
}

void FinishIndexMerge() {
  if (g_indexMergeDraw == kNoMerge) return;
  if (g_indexMergeDraw < g_capturing.draws.size() && !g_indexMergeList.empty()) {
    DrawCall& call = g_capturing.draws[g_indexMergeDraw];
    call.index_count = uint32_t(g_indexMergeList.size());
    call.index_data = g_capturing.alloc(call.index_count * 4, 4);
    call.index_blob.reset();
    std::memcpy(g_capturing.at(call.index_data), g_indexMergeList.data(), size_t(call.index_count) * 4);
  }
  g_indexMergeDraw = kNoMerge;
  g_indexMergeList.clear();
}

bool TryMergeIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, const ExpandedIndices& pre) {
  if (!REXCVAR_GET(kameo_gfx_merge_indexed)) return false;
  if (!pre.valid() || pre.blob->empty() || g_capturing.draws.empty()) return false;
  DrawCall& target = g_capturing.draws.back();
  // index_blob proves the target's own indices needed no rebasing, so this list shares its vertex space.
  if (target.kind != CommandKind::Draw || target.primitive != primitive || !target.index_blob || (!target.vertex_blob && target.vertex_source == kNoData) || !target.ps) return false;
  if (target.deferred_vertex_addr || target.index_count + pre.blob->size() > kMaxMergedIndices) return false;
  if (pre.max_vertex >= target.vertex_count) return false;
  const uint32_t stream0 = Load32(base, device + dev::kStreamBuffer);
  if (!stream0 || stream0 != target.stream0_object || !target.layout) return false;
  if ((Load32(base, stream0 + 12) & 0xFFFFFFFCu) != target.stream0_address || Load32(base, stream0 + 16) != target.stream0_fetch1) return false;
  if (Load32(base, device + dev::kStreamOffset) != target.stream0_offset || uint32_t(Load8(base, device + dev::kStreamStride) << dev::kStreamStrideShift) != target.stream0_stride) return false;
  if (!target.stream_key || StreamIdentity(base, device, *target.layout) != target.stream_key) return false;
  if (!MatchesTarget(base, device, target)) return false;
  const uint32_t index = uint32_t(g_capturing.draws.size() - 1);
  if (g_indexMergeDraw != index) {
    FinishIndexMerge();
    g_indexMergeList.assign(target.index_blob->begin(), target.index_blob->end());
    g_indexMergeDraw = index;
  }
  g_indexMergeList.insert(g_indexMergeList.end(), pre.blob->begin(), pre.blob->end());
  target.index_count = uint32_t(g_indexMergeList.size());
  g_mergedIndexedDraws.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool TryMergeBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t ring, uint32_t stride) {
  if (!REXCVAR_GET(kameo_gfx_merge_begin_vertices)) return false;
  const uint32_t unit = primitive == kPrimQuadList ? 4 : primitive == kPrimTriangleList ? 3 : 0;
  if (!unit || vertex_count % unit != 0) return false;
  if (g_pendingDeferred.size() != 1 || g_capturing.draws.empty() || g_pendingDeferred.back() != g_capturing.draws.size() - 1) return false;
  DrawCall& target = g_capturing.draws.back();
  if (target.kind != CommandKind::Draw || !target.deferred_vertex_addr || !target.ps || target.deferred_device != device || target.primitive != primitive || target.deferred_vertex_stride != stride) return false;
  if (target.deferred_vertex_count + vertex_count > kMaxMergedVertices) return false;
  if (!MatchesTarget(base, device, target)) return false;
  const uint32_t open_count = g_mergeDraw == g_pendingDeferred.back() ? g_mergeOpenCount : target.deferred_vertex_count;
  const uint8_t* open = GuestData(base, target.deferred_vertex_addr);
  g_mergeGather.insert(g_mergeGather.end(), open, open + size_t(open_count) * stride);
  g_mergeDraw = g_pendingDeferred.back();
  g_mergeOpenCount = vertex_count;
  target.deferred_vertex_addr = ring;
  target.deferred_vertex_count += vertex_count;
  target.vertex_count = target.deferred_vertex_count;
  target.vertex_bytes = target.deferred_vertex_count * target.layout->dst_stride;
  g_mergedDraws.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void ResetDrawMerges() {
  g_pendingDeferred.clear();
  g_mergeDraw = kNoMerge;
  g_mergeGather.clear();
  g_indexMergeDraw = kNoMerge;
  g_indexMergeList.clear();
}

}  // namespace kameo::gpu::detail
