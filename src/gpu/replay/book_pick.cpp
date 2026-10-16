#include "gpu/replay/book_pick.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_vulkan.h>
#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include "gpu/frame_ring.h"
#include "gpu/log.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pipeline_state.h"
#include "gpu/replay/replay_context.h"
#include "gpu/resolve.h"
#include "gpu/shaders/builtin_shaders.h"
#include "gpu/shaders/embedded_shaders.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_book_pick, true, "Kameo", "Read the storybook page under the mouse pointer back from the GPU, so hovering menu lines on the book stays exact while its pages move. Off falls back to a page mesh captured once.");

namespace kameo::gpu::detail {
namespace {

struct BookPickPass {
  std::unique_ptr<plume::RenderShader> ps;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<std::unique_ptr<plume::RenderBuffer>, kFramesInFlight> readback;
  std::array<bool, kFramesInFlight> pending = {};
  std::vector<BookPickDraw> draws;
  bool failed = false;
};
BookPickPass g_bookPick;

std::mutex g_bookPickMutex;
std::chrono::steady_clock::time_point g_bookPickRequested;
float g_bookPickPoint[2] = {-1.0f, -1.0f};
std::chrono::steady_clock::time_point g_bookPickAnswered;
bool g_bookPickHit = false;
float g_bookPickPage[2] = {0.0f, 0.0f};

bool BookPickWanted(float* point_x, float* point_y) {
  std::lock_guard<std::mutex> lock(g_bookPickMutex);
  *point_x = g_bookPickPoint[0];
  *point_y = g_bookPickPoint[1];
  return std::chrono::steady_clock::now() - g_bookPickRequested < std::chrono::milliseconds(250) && *point_x >= 0.0f && *point_y >= 0.0f;
}

plume::RenderPipeline* EnsureBookPickPipeline(GraphicsSystem::DrawPathContext& ctx, const BookPickDraw& draw) {
  const DrawCall& call = *draw.call;
  const plume::RenderPrimitiveTopology topology = HostTopology(call.primitive);
  const uint64_t key = (reinterpret_cast<uintptr_t>(draw.vs) * 1099511628211ull) ^ (call.layout->key << 1) ^ (uint64_t(call.layout->dst_stride) << 48) ^ uint64_t(topology);
  if (auto found = g_bookPick.pipelines.find(key); found != g_bookPick.pipelines.end()) return found->second.get();
  std::vector<plume::RenderInputElement> elements;
  for (const HostElement& e : call.layout->elements) elements.emplace_back(DeclUsageSemantic(e.usage), e.usage_index, DeclUsageLocation(e.usage, e.usage_index), e.format, 0, e.dst_offset);
  const plume::RenderInputSlot slot(0, call.layout->dst_stride);
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = ctx.layout;
  desc.vertexShader = draw.vs;
  desc.pixelShader = g_bookPick.ps.get();
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements.data();
  desc.inputElementsCount = uint32_t(elements.size());
  desc.primitiveTopology = topology;
  desc.renderTargetFormat[0] = plume::RenderFormat::R32G32B32A32_FLOAT;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.cullMode = plume::RenderCullMode::NONE;
  if (ctx.spirv) {
    desc.specConstants = kGuestSpecConstants;
    desc.specConstantsCount = 1;
  }
  auto pipeline = ctx.device->createGraphicsPipeline(desc);
  if (!PipelineCreated(pipeline.get())) pipeline.reset();
  return g_bookPick.pipelines.emplace(key, std::move(pipeline)).first->second.get();
}

}  // namespace

bool BeginBookPickFrame() {
  float book_pick_x = 0.0f;
  float book_pick_y = 0.0f;
  const bool wanted = REXCVAR_GET(kameo_gfx_book_pick) && !g_bookPick.failed && BookPickWanted(&book_pick_x, &book_pick_y);
  g_bookPick.draws.clear();
  return wanted;
}

bool IsBookPageTexture(uint32_t heap_index) {
  for (const auto& [key, entry] : g_resolveTargets) {
    if (entry.heap_index == heap_index) return !key.depth && entry.width == 640 && entry.height == 480;
  }
  return false;
}

bool BookPickHasRoom() { return g_bookPick.draws.size() < 32; }

void AddBookPickDraw(const BookPickDraw& draw) { g_bookPick.draws.push_back(draw); }

void DrawBookPick(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const plume::RenderFramebuffer* scene_framebuffer, uint32_t width, uint32_t height) {
  const uint32_t slot = ctx.frame_slot % kFramesInFlight;
  if (g_bookPick.pending[slot] && g_bookPick.readback[slot]) {
    g_bookPick.pending[slot] = false;
    if (const auto* texel = static_cast<const float*>(g_bookPick.readback[slot]->map())) {
      std::lock_guard<std::mutex> lock(g_bookPickMutex);
      g_bookPickHit = texel[2] > 0.5f;
      g_bookPickPage[0] = texel[0] * 640.0f;
      g_bookPickPage[1] = texel[1] * 480.0f;
      g_bookPickAnswered = std::chrono::steady_clock::now();
      g_bookPick.readback[slot]->unmap();
    }
  }
  float point_x = 0.0f;
  float point_y = 0.0f;
  const bool wanted = BookPickWanted(&point_x, &point_y);
  if (!wanted || g_bookPick.draws.empty() || g_bookPick.failed) {
    g_bookPick.draws.clear();
    return;
  }
  if (!g_bookPick.ps) {
    g_bookPick.ps = ctx.spirv ? ctx.device->createShader(shaders::g_bookPickPSSpirv, sizeof(shaders::g_bookPickPSSpirv), "PSMain", plume::RenderShaderFormat::SPIRV) : ctx.device->createShader(shaders::g_bookPickPSDxil, sizeof(shaders::g_bookPickPSDxil), "PSMain", plume::RenderShaderFormat::DXIL);
    if (!g_bookPick.ps) {
      KGFX_ERROR("book pick shader creation failed");
      g_bookPick.failed = true;
      return;
    }
  }
  if (!g_bookPick.texture || g_bookPick.width != width || g_bookPick.height != height) {
    g_bookPick.framebuffer.reset();
    g_bookPick.texture = ctx.device->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, plume::RenderFormat::R32G32B32A32_FLOAT));
    if (!g_bookPick.texture) {
      KGFX_ERROR("book pick target {}x{} creation failed", width, height);
      g_bookPick.failed = true;
      return;
    }
    const plume::RenderTexture* attachment = g_bookPick.texture.get();
    plume::RenderFramebufferDesc fb;
    fb.colorAttachments = &attachment;
    fb.colorAttachmentsCount = 1;
    g_bookPick.framebuffer = ctx.device->createFramebuffer(fb);
    g_bookPick.width = width;
    g_bookPick.height = height;
  }
  if (!g_bookPick.readback[slot]) g_bookPick.readback[slot] = ctx.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(64));
  if (!g_bookPick.framebuffer || !g_bookPick.readback[slot]) {
    g_bookPick.failed = true;
    return;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(g_bookPick.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(g_bookPick.framebuffer.get());
  list->setScissors(plume::RenderRect(0, 0, width, height));
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  BindSceneDescriptors(list, ctx);
  for (const BookPickDraw& draw : g_bookPick.draws) {
    plume::RenderPipeline* pipeline = EnsureBookPickPipeline(ctx, draw);
    if (!pipeline) continue;
    list->setPipeline(pipeline);
    if (ctx.spirv) {
      const uint64_t ring = g_constantRing.buffer->getDeviceAddress();
      const uint64_t push[3] = {ring + draw.vs_offset, ring + draw.ps_offset, ring + draw.shared_offset};
      list->setGraphicsPushConstants(0, push, 0, uint32_t(sizeof(push)));
    } else {
      list->setGraphicsRootDescriptor(g_constantRing.buffer->at(draw.vs_offset), 0);
      list->setGraphicsRootDescriptor(g_constantRing.buffer->at(draw.ps_offset), 1);
      list->setGraphicsRootDescriptor(g_constantRing.buffer->at(draw.shared_offset), 2);
    }
    const plume::RenderVertexBufferView vertex_view(draw.vertex_buffer->at(draw.vertex_offset), draw.call->vertex_bytes);
    const plume::RenderInputSlot input(0, draw.call->vertex_stride);
    list->setVertexBuffers(0, &vertex_view, 1, &input);
    const plume::RenderIndexBufferView index_view(draw.index_buffer->at(draw.index_offset), draw.call->index_count * 4, plume::RenderFormat::R32_UINT);
    list->setIndexBuffer(&index_view);
    list->setViewports(plume::RenderViewport(draw.viewport[0], draw.viewport[1], draw.viewport[2], draw.viewport[3], draw.viewport[4], draw.viewport[5]));
    list->drawIndexedInstanced(draw.call->index_count, 1, 0, 0, 0);
  }
  g_bookPick.draws.clear();
  const int32_t px = std::clamp(int32_t(point_x * float(width)), 0, int32_t(width) - 1);
  const int32_t py = std::clamp(int32_t(point_y * float(height)), 0, int32_t(height) - 1);
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(g_bookPick.texture.get(), plume::RenderTextureLayout::COPY_SOURCE));
  if (ctx.spirv) {
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {px, py, 0};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList*>(list)->vk, static_cast<plume::VulkanTexture*>(g_bookPick.texture.get())->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<plume::VulkanBuffer*>(g_bookPick.readback[slot].get())->vk, 1, &region);
    g_bookPick.pending[slot] = true;
  }
#ifdef _WIN32
  else {
    // plume's copyTextureRegion sets sample positions on its destination texture, which a readback footprint does not have, so the copy goes to D3D12 directly.
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = static_cast<plume::D3D12Buffer*>(g_bookPick.readback[slot].get())->d3d;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = 0;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    destination.PlacedFootprint.Footprint.Width = 1;
    destination.PlacedFootprint.Footprint.Height = 1;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = 16;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = static_cast<plume::D3D12Texture*>(g_bookPick.texture.get())->d3d;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    const D3D12_BOX box = {UINT(px), UINT(py), 0, UINT(px + 1), UINT(py + 1), 1};
    static_cast<plume::D3D12CommandList*>(list)->d3d->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    g_bookPick.pending[slot] = true;
  }
#endif
  list->setFramebuffer(scene_framebuffer);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  list->setScissors(plume::RenderRect(0, 0, width, height));
}

void ReleaseBookPick() { g_bookPick = BookPickPass(); }

}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void RequestBookPick(float window_x, float window_y) {
  std::lock_guard<std::mutex> lock(g_bookPickMutex);
  g_bookPickRequested = std::chrono::steady_clock::now();
  g_bookPickPoint[0] = window_x;
  g_bookPickPoint[1] = window_y;
}

bool LatestBookPick(float* page_x, float* page_y, bool* hit) {
  std::lock_guard<std::mutex> lock(g_bookPickMutex);
  if (std::chrono::steady_clock::now() - g_bookPickAnswered > std::chrono::milliseconds(150)) return false;
  *hit = g_bookPickHit;
  *page_x = g_bookPickPage[0];
  *page_y = g_bookPickPage[1];
  return true;
}

}  // namespace kameo::gpu
