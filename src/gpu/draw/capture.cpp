#include "gpu/draw/capture.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include <fmt/format.h>

#include "gpu/config.h"
#include "gpu/constant_buffers.h"
#include "gpu/draw/draw_merge.h"
#include "gpu/draw/indices.h"
#include "gpu/draw/render_state.h"
#include "gpu/draw/vertex_capture.h"
#include "gpu/draw_path.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"
#include "gpu/log.h"
#include "gpu/occlusion.h"
#include "gpu/vertex_declaration.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_stale_pixel_shader, true, "Kameo", "Draw colour draws that bind no pixel shader with the last one bound, as the console's GPU does; this is how fading models such as an absorbed baby warrior draw. Off drops them.");

namespace kameo::gpu::detail {

std::mutex g_captureMutex;
DrawFrame g_capturing;
DrawFrame g_submitting;
DrawFrameExchange g_frameExchange;
std::atomic<uint64_t> g_framesTaken{0};
uint64_t g_captureFrame = 0;
std::vector<uint32_t> g_frameGuestReads;

namespace {

// GeneratingShadowMap: preShadowRender (0x82260a30) sets it and postShadowRender (0x82260cb8) clears it, the authoritative "these draws are a shadow map" signal.
constexpr uint32_t kGeneratingShadowMap = 0x82B70B8Fu;
constexpr uint32_t kRbModeControl = 0x2D94;  // RB_MODECONTROL in the register shadow

// drawLoadFixedFunctionVertexShader keeps each fixed-function vertex shader object at +8 of a 16-byte record, and fontResetGraphics binds the font's own; both transform by the matrix in c0-c3.
constexpr uint32_t kFixedFunctionVertexShaders = 0x82753DE8u;
constexpr uint32_t kFixedFunctionVertexShaderCount = 8;
constexpr uint32_t kFontVertexShader = 0x8276FACCu;

uint32_t g_lastGuestReadObject = 0;
// The last pixel shader a captured draw bound, which the GPU keeps running for draws that bind none.
uint32_t g_lastPixelShaderObject = 0;
const ShaderCacheEntry* g_lastPixelShader = nullptr;

// Shared by the draw entry points: resolves the shaders, layout and index range, then records the draw.
void CaptureCommon(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t first_vertex, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count, const StreamOverride& override_stream = {}, uint32_t deferred_vertex_addr = 0, uint32_t deferred_vertex_stride = 0, const ExpandedIndices* pre_expanded = nullptr, uint32_t guest_caller = 0, uint32_t guest_command = 0, uint32_t guest_command_stream = 0, const uint32_t* texture_objects = nullptr) {
  // Casters skipped while GeneratingShadowMap is set leave the map at its cleared far depth.
  if (!REXCVAR_GET(kameo_gfx_shadows) && Load8(base, kGeneratingShadowMap) != 0) return;
  const uint32_t vs_object = Load32(base, device + kDeviceVertexShader);
  uint32_t ps_object = Load32(base, device + kDevicePixelShader);
  const ShaderCacheEntry* vs = nullptr;
  const ShaderCacheEntry* ps = nullptr;
  {
    vs = LookupShaderBinding(vs_object);
    ps = LookupShaderBinding(ps_object);
  }
  // With no pixel shader bound the GPU keeps drawing colour with the last one loaded unless RB_MODECONTROL is depth-only (5); fading models draw this way.
  if (ps) {
    g_lastPixelShaderObject = ps_object;
    g_lastPixelShader = ps;
  } else if (ps_object == 0 && vs && g_lastPixelShader && REXCVAR_GET(kameo_gfx_stale_pixel_shader) && Load8(base, kGeneratingShadowMap) == 0 && g_openOcclusionQuery.load(std::memory_order_relaxed) == 0 && (Load32(base, device + kRbModeControl) & 7) != 5) {
    ps_object = g_lastPixelShaderObject;
    ps = g_lastPixelShader;
  }
  if (!vs || !ps) {
    // A null pixel shader is depth-only only while GeneratingShadowMap is set or an occlusion query is open: grass_initRender clears the slot for an ordinary colour pass.
    const bool generating_shadow = Load8(base, kGeneratingShadowMap) != 0;
    if (vs && ps_object == 0 && (generating_shadow || g_openOcclusionQuery.load(std::memory_order_relaxed) != 0)) {
    } else {
      if (vs_object == 0) {
      } else if (ps_object == 0) {
      } else {
      }
      return;
    }
  }

  const uint32_t decl = Load32(base, device + kDeviceVertexDeclaration);
  if (!decl) {
    return;
  }
  const HostLayout* layout = nullptr;
  {
    layout = GetLayout(base, decl);
  }
  if (!layout) {
    return;
  }

  // Reused across draws: a per-draw vector was a malloc and a free on each of ~4800 draws a frame.
  static thread_local std::vector<uint32_t> expanded;
  uint32_t min_vertex = 0;
  uint32_t max_vertex = 0;
  // Either the caller already has this list from the index cache, or it is built here.
  const std::vector<uint32_t>* expanded_list = nullptr;
  {
    if (pre_expanded && pre_expanded->valid()) {
      expanded_list = pre_expanded->blob.get();
      min_vertex = pre_expanded->min_vertex;
      max_vertex = pre_expanded->max_vertex;
    } else {
      if (!ExpandTopology(primitive, indices ? index_count : vertex_count, indices, &expanded,
                          &min_vertex, &max_vertex)) {
        return;
      }
      expanded_list = &expanded;
    }
  }
  if (expanded_list->empty()) {
    return;
  }

  // Indices are relative to the stream; StartVertex shifts them for the non-indexed path, where `expanded` holds 0..count-1.
  min_vertex += first_vertex;
  max_vertex += first_vertex;
  if (max_vertex - min_vertex + 1 > 1u << 20) {
    return;
  }

  std::unique_lock<std::mutex> lock(g_captureMutex, std::defer_lock);
  {
    lock.lock();
  }
  g_capturing.guest_base = base;
  ResolveDeferredVertices(base);
  FinishIndexMerge();
  DrawFrame& frame = g_capturing;

  // Built in place: a DrawCall is over a kilobyte, and moving one into the frame copied all of it on every draw.
  DrawCall& call = frame.draws.emplace_back();
  call.primitive = primitive;
  call.guest_caller = guest_caller;
  call.guest_command = guest_command;
  call.guest_command_stream = guest_command_stream;
  call.guest_decl = decl;
  call.vs_object = vs_object;
  call.ps_object = ps_object;
  if (vs_object) {
    call.hud_2d = Load32(base, kFontVertexShader) == vs_object;
    for (uint32_t i = 0; i < kFixedFunctionVertexShaderCount && !call.hud_2d; ++i) call.hud_2d = Load32(base, kFixedFunctionVertexShaders + 16 * i) == vs_object;
  }
  call.vs = vs;
  call.ps = ps;
  call.depth_only = (ps == nullptr);
  call.layout = layout;
  call.vertex_stride = layout->dst_stride;
  float vertex_viewport[4];
  for (uint32_t i = 0; i < 4; ++i) {
    vertex_viewport[i] = float(Load32(base, device + dev::kViewport + 4 * i));
  }

  if (!override_stream.active() && !deferred_vertex_addr) {
    call.stream_key = StreamIdentity(base, device, *layout);
    call.stream0_object = Load32(base, device + dev::kStreamBuffer);
    if (call.stream0_object) {
      const uint32_t fetch0 = Load32(base, call.stream0_object + 12);
      call.stream0_address = fetch0 & 0xFFFFFFFCu;
      call.stream0_fetch1 = Load32(base, call.stream0_object + 16);
      call.stream0_offset = Load32(base, device + dev::kStreamOffset);
      call.stream0_stride = Load8(base, device + dev::kStreamStride) << dev::kStreamStrideShift;
    }
  }

  uint32_t index_base = 0;
  if (deferred_vertex_addr) {
    // Everything except the vertices, which are read at frame end.
    call.deferred_vertex_addr = deferred_vertex_addr;
    call.deferred_vertex_stride = deferred_vertex_stride;
    call.deferred_device = device;
    call.deferred_vertex_count = vertex_count;
    call.vertex_count = vertex_count;
    call.vertex_bytes = vertex_count * layout->dst_stride;
  } else if (!CaptureVertices(base, device, *layout, min_vertex, max_vertex, &frame,
                              &call.vertex_data, &call.vertex_bytes, &index_base,
                              &call.vertex_count, &call.vertex_blob, vertex_viewport,
                              override_stream, &call.vertex_source)) {
    frame.draws.pop_back();
    return;
  }


  {
    call.index_count = uint32_t(expanded_list->size());
    const uint32_t shift = first_vertex - index_base;
    if (pre_expanded && pre_expanded->valid() && shift == 0) {
      // A range beginning at vertex zero can use the cached indices directly.
      call.index_blob = pre_expanded->blob;
      call.index_data = kNoData;
    } else {
      call.index_data = frame.alloc(call.index_count * 4, 4);
      auto* dst = reinterpret_cast<uint32_t*>(frame.at(call.index_data));
      const uint32_t* src = expanded_list->data();
      for (uint32_t i = 0; i < call.index_count; ++i) {
        dst[i] = src[i] + shift;
      }
    }
  }

  call.vs_constants = CaptureConstants(base, device, &g_vsDedup, &frame, ShaderConstantRangeOf(vs), &call.vs_constant_bytes);
  call.ps_constants = CaptureConstants(base, device, &g_psDedup, &frame, ShaderConstantRangeOf(ps), &call.ps_constant_bytes);
  {
    CaptureState(base, device, &call);
  }
  if (texture_objects) std::memcpy(call.texture_objects, texture_objects, sizeof(call.texture_objects));
  for (uint32_t s = 0; s < kSamplerCount; ++s) {
    const uint32_t object = call.texture_objects[s];
    call.texture_addresses[s] = object ? (Load32(base, object + 20) & 0xFFFFF000u) : 0;
    if (!object || object == g_lastGuestReadObject || !call.textures[s].valid()) continue;
    // Recording reads guest memory only for textures: the object header, the texels and the mip chain, as EnsureTexture and CollectGuestMips do.
    g_lastGuestReadObject = object;
    const uint32_t texels = call.texture_addresses[s];
    g_frameGuestReads.push_back(CanonicalGuestAddress(object));
    g_frameGuestReads.push_back(CanonicalGuestAddress(texels));
    const uint32_t fetch_base = call.textures[s].address();
    const uint32_t fetch_mip = call.textures[s].mip_address();
    if (texels && fetch_mip > fetch_base) g_frameGuestReads.push_back(CanonicalGuestAddress(texels + (fetch_mip - fetch_base)));
  }
  call.stencil_only = call.depth_only && call.rt_surface != 0 && (call.stencil_enable || g_openOcclusionQuery.load(std::memory_order_relaxed) != 0);
  if (call.stencil_only) call.depth_only = false;
  {
    if (call.deferred_vertex_addr != 0) {
      g_pendingDeferred.push_back(uint32_t(frame.draws.size() - 1));
    }
  }
}

}  // namespace
}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void CaptureIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, int32_t base_vertex, uint32_t start_index, uint32_t index_count, uint32_t guest_caller, uint32_t guest_command, uint32_t guest_command_stream) {
  const uint32_t ib = Load32(base, device + dev::kIndexBuffer);
  if (!ib || index_count == 0) {
    return;
  }
  // Index width is bit 31 of the buffer's Common word: DrawIndexedVertices takes StartIndex*4 when it is set and StartIndex*2 when not.
  const bool index32 = (Load32(base, ib) & 0x80000000u) != 0;
  const uint32_t address = Load32(base, ib + 12) & 0xFFFFFFFCu;
  if (!address) {
    return;
  }

  static thread_local std::vector<uint32_t> indices;
  ExpandedIndices pre;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    ReadIndexedDraw(base, device, primitive, base_vertex, address, start_index, index_count, index32, &indices, &pre);
    if (TryMergeIndexedDraw(base, device, primitive, pre)) return;
  }

  // A miss that failed to expand falls back to the uncached path, which reports the reason through its own counter.
  CaptureCommon(base, device, primitive, 0, 0, pre.valid() ? nullptr : indices.data(), index_count, {}, 0, 0, pre.valid() ? &pre : nullptr, guest_caller, guest_command, guest_command_stream);
}

void CaptureDraw(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count) {
  if (vertex_count == 0) {
    return;
  }
  CaptureCommon(base, device, primitive, start_vertex, vertex_count, nullptr, 0);
}

// D3DDevice_BeginVertices hands the guest a ring pointer and the inlined EndVertices is not hookable, so the state is captured now and the geometry at the next captured call; this is the whole 2D icon system.
void CaptureDrawBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t ring, uint32_t stride) {
  if (!ring || !stride || !vertex_count) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (TryMergeBeginVertices(base, device, primitive, vertex_count, ring, stride)) return;
  }
  CaptureCommon(base, device, primitive, 0, vertex_count, nullptr, 0, {}, ring, stride);
}

void CaptureDrawUP(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t data, uint32_t stride) {
  if (!data || !stride || !vertex_count) {
    return;
  }
  StreamOverride override_stream;
  override_stream.data = GuestData(base, data);
  override_stream.stride = stride;
  override_stream.count = vertex_count;
  CaptureCommon(base, device, primitive, 0, vertex_count, nullptr, 0, override_stream);
}

void CaptureDrawIndexedUP(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t vertex_data, uint32_t stride, uint32_t index_count, uint32_t index_data, const uint32_t* texture_objects) {
  if (!vertex_data || !index_data || !stride || !vertex_count || !index_count) {
    return;
  }
  static thread_local std::vector<uint32_t> indices;
  indices.resize(index_count);
  const uint8_t* src = GuestData(base, index_data);
  const std::optional<uint32_t> reset = GuestStripReset(base, device, primitive, false);
  for (uint32_t i = 0; i < index_count; ++i) {
    uint16_t value;
    std::memcpy(&value, src + 2 * i, 2);
    indices[i] = Swap16(value);
    if (reset && indices[i] == *reset) indices[i] = kStripRestart;
  }
  StreamOverride override_stream;
  override_stream.data = GuestData(base, vertex_data);
  override_stream.stride = stride;
  override_stream.count = vertex_count;
  CaptureCommon(base, device, primitive, 0, vertex_count, indices.data(), index_count, override_stream, 0, 0, nullptr, 0, 0, 0, texture_objects);
}

uint32_t SubmitCapturedFrame(std::vector<uint32_t>* guest_reads) {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  // The guest has finished writing every BeginVertices ring by now, so those vertices are read before the frame is handed over and the arena stays self-contained.
  ResolveDeferredVertices(g_capturing.guest_base);
  FinishIndexMerge();
  // Drop anything the guest has not drawn for 240 frames, or streaming a large level grows the caches without limit.
  ++g_captureFrame;
  if ((g_captureFrame % 240) == 0) {
    size_t live = EvictTranscodeCache(g_captureFrame);
    live += EvictIndexCache(g_captureFrame);
  }
  guest_reads->swap(g_frameGuestReads);
  g_frameGuestReads.clear();
  g_frameGuestReads.reserve(guest_reads->size());
  g_lastGuestReadObject = 0;
  const uint32_t draws = uint32_t(g_capturing.draws.size());
  if (g_frameExchange.publish(g_capturing)) {
  }
  g_vsDedup.reset();
  g_psDedup.reset();
  ClearVertexReuse();
  return draws;
}

}  // namespace kameo::gpu
