#include "gpu/occlusion.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_vulkan.h>
#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include "gpu/draw/capture.h"
#include "gpu/draw/draw_constants.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {

std::atomic<uint32_t> g_openOcclusionQuery{0};

namespace {

struct OcclusionQueryState {
  uint64_t end_seq = 0;
  uint64_t end_frame = 0;
  uint64_t result_seq = 0;
  uint32_t result = 0;
};
constexpr uint64_t kOcclusionTimeoutFrames = 16;
constexpr uint32_t kFakeOcclusionSamples = 1000;
std::atomic<bool> g_occlusionHeapReady{false};
std::mutex g_occlusionMutex;
std::unordered_map<uint32_t, OcclusionQueryState> g_occlusionQueries;
uint64_t g_occlusionSeq = 0;

struct OcclusionReadback {
  uint32_t query = 0;
  uint64_t seq = 0;
  uint32_t index = 0;
};

struct OcclusionSlot {
  std::unique_ptr<plume::RenderBuffer> readback;
  std::vector<OcclusionReadback> pending;
  double scale = 1.0;
};

OcclusionSlot g_occlusionSlots[kFramesInFlight];
bool g_occlusionHeapFailed = false;
VkQueryPool g_vkOcclusionPool[kFramesInFlight] = {VK_NULL_HANDLE};
VkDevice g_vkOcclusionDevice = VK_NULL_HANDLE;
#ifdef _WIN32
ID3D12QueryHeap* g_occlusionHeap = nullptr;
#endif

bool EnsureOcclusionHeap(const GraphicsSystem::DrawPathContext& ctx) {
  if (g_occlusionHeapFailed) return false;
  if (ctx.spirv) {
    auto* vk_device = static_cast<plume::VulkanDevice*>(ctx.device);
    g_vkOcclusionDevice = vk_device->vk;
    for (uint32_t f = 0; f < kFramesInFlight; ++f) {
      if (g_vkOcclusionPool[f] != VK_NULL_HANDLE) continue;
      VkQueryPoolCreateInfo desc = {};
      desc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      desc.queryType = VK_QUERY_TYPE_OCCLUSION;
      desc.queryCount = kOcclusionQueriesPerFrame;
      if (vkCreateQueryPool(vk_device->vk, &desc, nullptr, &g_vkOcclusionPool[f]) != VK_SUCCESS) {
        g_occlusionHeapFailed = true;
        KGFX_WARN("vulkan occlusion query pool could not be created; light glows will not be occluded");
        return false;
      }
    }
    g_occlusionHeapReady.store(true, std::memory_order_release);
  } else {
#ifdef _WIN32
    if (g_occlusionHeap == nullptr) {
      D3D12_QUERY_HEAP_DESC desc = {};
      desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
      desc.Count = kOcclusionQueriesPerFrame * kFramesInFlight;
      if (FAILED(static_cast<plume::D3D12Device*>(ctx.device)->d3d->CreateQueryHeap(&desc, IID_PPV_ARGS(&g_occlusionHeap)))) {
        g_occlusionHeap = nullptr;
        g_occlusionHeapFailed = true;
        KGFX_WARN("occlusion query heap could not be created; light glows will not be occluded");
        return false;
      }
      g_occlusionHeapReady.store(true, std::memory_order_release);
    }
#else
    return false;
#endif
  }
  OcclusionSlot& slot = g_occlusionSlots[ctx.frame_slot];
  if (!slot.readback) slot.readback = ctx.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(sizeof(uint64_t) * kOcclusionQueriesPerFrame));
  return slot.readback != nullptr;
}

// The draw loop's queries, so a texture upload recorded mid-draw can close one before its barrier ends the render pass.
const GraphicsSystem::DrawPathContext* g_openQueriesCtx = nullptr;
OcclusionReplay* g_openQueries = nullptr;

}  // namespace

void ResetVulkanOcclusionPool(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list) {
  if (ctx.spirv && EnsureOcclusionHeap(ctx)) {
    auto* vk_list = static_cast<plume::VulkanCommandList*>(list);
    vk_list->endActiveRenderPass();
    vkCmdResetQueryPool(vk_list->vk, g_vkOcclusionPool[ctx.frame_slot], 0, kOcclusionQueriesPerFrame);
  }
}

void ClearPendingOcclusion(uint32_t frame_slot) { g_occlusionSlots[frame_slot].pending.clear(); }

void ReplayOcclusionQuery(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const DrawCall& call, OcclusionReplay& replay) {
  if (!EnsureOcclusionHeap(ctx)) return;
  const bool begin = call.kind == CommandKind::QueryBegin;
  if (ctx.spirv) {
    // Each frame slot has a Vulkan pool of its own, so indices start at zero; a query has to sit inside the render pass its draws are recorded in.
    auto* vk_list = static_cast<plume::VulkanCommandList*>(list);
    if (replay.active && (begin || replay.active_query == call.query_object)) {
      vkCmdEndQuery(vk_list->vk, g_vkOcclusionPool[ctx.frame_slot], replay.active_index);
      replay.active = false;
      if (!begin) g_occlusionSlots[ctx.frame_slot].pending.push_back({call.query_object, call.query_seq, replay.active_index});
    } else if (!begin && !replay.active && replay.closed_query == call.query_object) {
      g_occlusionSlots[ctx.frame_slot].pending.push_back({call.query_object, call.query_seq, replay.closed_index});
    }
    replay.closed_query = 0;
    if (begin && replay.used < kOcclusionQueriesPerFrame) {
      replay.active_index = replay.used++;
      replay.active_query = call.query_object;
      replay.active = true;
      if (vk_list->targetFramebuffer != nullptr) vk_list->checkActiveRenderPass();
      vkCmdBeginQuery(vk_list->vk, g_vkOcclusionPool[ctx.frame_slot], replay.active_index, 0);
    }
    return;
  }
#ifdef _WIN32
  ID3D12GraphicsCommandList* d3d = static_cast<plume::D3D12CommandList*>(list)->d3d;
  if (replay.active && (begin || replay.active_query == call.query_object)) {
    d3d->EndQuery(g_occlusionHeap, D3D12_QUERY_TYPE_OCCLUSION, replay.active_index);
    replay.active = false;
    if (!begin) g_occlusionSlots[ctx.frame_slot].pending.push_back({call.query_object, call.query_seq, replay.active_index - ctx.frame_slot * kOcclusionQueriesPerFrame});
  }
  if (begin && replay.used < kOcclusionQueriesPerFrame) {
    replay.active_index = ctx.frame_slot * kOcclusionQueriesPerFrame + replay.used++;
    replay.active_query = call.query_object;
    replay.active = true;
    d3d->BeginQuery(g_occlusionHeap, D3D12_QUERY_TYPE_OCCLUSION, replay.active_index);
  }
#endif
}

void SuspendVulkanQuery(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, OcclusionReplay& replay) {
  if (!ctx.spirv || !replay.active) return;
  vkCmdEndQuery(static_cast<plume::VulkanCommandList*>(list)->vk, g_vkOcclusionPool[ctx.frame_slot], replay.active_index);
  replay.active = false;
  replay.closed_query = replay.active_query;
  replay.closed_index = replay.active_index;
}

void SuspendOpenQuery(plume::RenderCommandList* list) {
  if (g_openQueries != nullptr) SuspendVulkanQuery(*g_openQueriesCtx, list, *g_openQueries);
}

OpenQueriesScope::OpenQueriesScope(const GraphicsSystem::DrawPathContext& ctx, OcclusionReplay& replay) {
  g_openQueriesCtx = &ctx;
  g_openQueries = &replay;
}

OpenQueriesScope::~OpenQueriesScope() { g_openQueries = nullptr; }

void FinishOcclusionQueries(const GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, OcclusionReplay& replay, uint32_t width, uint32_t height) {
  if (replay.used == 0) return;
  OcclusionSlot& slot = g_occlusionSlots[ctx.frame_slot];
  // The guest counts pixels of a single-sample 1280x720 frame; the queries here count every sample of the scaled, multisampled scene.
  slot.scale = (double(kGuestWidth) * kGuestHeight) / (double(std::max(width, 1u)) * std::max(height, 1u) * std::max(ctx.scene_samples, 1u));
  if (ctx.spirv) {
    auto* vk_list = static_cast<plume::VulkanCommandList*>(list);
    if (replay.active) {
      vkCmdEndQuery(vk_list->vk, g_vkOcclusionPool[ctx.frame_slot], replay.active_index);
      replay.active = false;
    }
    vk_list->endActiveRenderPass();
    vkCmdCopyQueryPoolResults(vk_list->vk, g_vkOcclusionPool[ctx.frame_slot], 0, replay.used, static_cast<plume::VulkanBuffer*>(slot.readback.get())->vk, 0, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return;
  }
#ifdef _WIN32
  ID3D12GraphicsCommandList* d3d = static_cast<plume::D3D12CommandList*>(list)->d3d;
  if (replay.active) {
    d3d->EndQuery(g_occlusionHeap, D3D12_QUERY_TYPE_OCCLUSION, replay.active_index);
    replay.active = false;
  }
  d3d->ResolveQueryData(g_occlusionHeap, D3D12_QUERY_TYPE_OCCLUSION, ctx.frame_slot * kOcclusionQueriesPerFrame, replay.used, static_cast<plume::D3D12Buffer*>(slot.readback.get())->d3d, 0);
#endif
}

void ReleaseOcclusion() {
  for (OcclusionSlot& slot : g_occlusionSlots) {
    slot.readback.reset();
    slot.pending.clear();
  }
  if (g_vkOcclusionDevice != VK_NULL_HANDLE) {
    for (VkQueryPool& pool : g_vkOcclusionPool) {
      if (pool != VK_NULL_HANDLE) vkDestroyQueryPool(g_vkOcclusionDevice, pool, nullptr);
      pool = VK_NULL_HANDLE;
    }
    g_vkOcclusionDevice = VK_NULL_HANDLE;
  }
#ifdef _WIN32
  if (g_occlusionHeap != nullptr) {
    g_occlusionHeap->Release();
    g_occlusionHeap = nullptr;
  }
#endif
  g_occlusionHeapFailed = false;
  g_occlusionHeapReady.store(false, std::memory_order_release);
  {
    std::lock_guard lock(g_occlusionMutex);
    g_occlusionQueries.clear();
  }
}

}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void CaptureOcclusionQuery(uint8_t* base, uint32_t query, bool begin) {
  DrawCall call;
  call.kind = begin ? CommandKind::QueryBegin : CommandKind::QueryEnd;
  call.query_object = query;
  std::lock_guard<std::mutex> lock(g_captureMutex);
  if (begin) {
    g_openOcclusionQuery.store(query, std::memory_order_relaxed);
  } else {
    g_openOcclusionQuery.compare_exchange_strong(query, 0u, std::memory_order_relaxed);
    call.query_seq = ++g_occlusionSeq;
    std::lock_guard<std::mutex> results(g_occlusionMutex);
    OcclusionQueryState& state = g_occlusionQueries[call.query_object];
    state.end_seq = call.query_seq;
    state.end_frame = g_captureFrame;
  }
  g_capturing.guest_base = base;
  g_capturing.draws.push_back(call);
}

bool OcclusionQueryResult(uint32_t query, uint32_t* samples) {
  // Without a query heap (Vulkan) every query reports a fixed count as Xenia's query_occlusion_fake_sample_count does.
  if (!g_occlusionHeapReady.load(std::memory_order_acquire)) {
    *samples = kFakeOcclusionSamples;
    return true;
  }
  std::lock_guard<std::mutex> lock(g_occlusionMutex);
  auto it = g_occlusionQueries.find(query);
  if (it == g_occlusionQueries.end()) {
    *samples = 0;
    return true;
  }
  const OcclusionQueryState& state = it->second;
  // A frame the exchange dropped never reaches the GPU, so a query stuck that long settles for its last answer.
  if (state.result_seq < state.end_seq && g_captureFrame - state.end_frame < kOcclusionTimeoutFrames) return false;
  *samples = state.result;
  return true;
}

void PublishOcclusionResults(uint32_t frame_slot, bool retired) {
  OcclusionSlot& slot = g_occlusionSlots[frame_slot % kFramesInFlight];
  if (slot.pending.empty()) return;
  const auto* counts = retired && slot.readback ? static_cast<const uint64_t*>(slot.readback->map()) : nullptr;
  if (counts) {
    std::lock_guard<std::mutex> lock(g_occlusionMutex);
    for (const OcclusionReadback& r : slot.pending) {
      auto it = g_occlusionQueries.find(r.query);
      if (it == g_occlusionQueries.end() || it->second.result_seq > r.seq) continue;
      it->second.result_seq = r.seq;
      it->second.result = uint32_t(std::min(std::ceil(double(counts[r.index]) * slot.scale), 4294967295.0));
    }
    slot.readback->unmap();
  }
  slot.pending.clear();
}

}  // namespace kameo::gpu
