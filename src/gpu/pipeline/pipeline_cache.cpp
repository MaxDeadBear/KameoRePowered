#include "gpu/pipeline/pipeline_cache.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include <plume_vulkan.h>
#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include "gpu/config.h"
#include "gpu/guest_state.h"
#include "gpu/log.h"
#include "gpu/pipeline/pipeline_state.h"
#include "gpu/shaders/embedded_shaders.h"

namespace kameo::gpu::detail {

uint32_t g_pipelineFailures = 0;

namespace {

std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> g_pipelines;

}  // namespace

bool PipelineCreated(plume::RenderPipeline* pipeline) {
  if (!pipeline) {
    return false;
  }
  auto* gfx = GraphicsSystem::instance();
  if (gfx && gfx->using_vulkan()) {
    auto* vk = static_cast<plume::VulkanGraphicsPipeline*>(pipeline);
    return vk->vk != VK_NULL_HANDLE;
  }
#ifdef _WIN32
  auto* d3d12 = static_cast<plume::D3D12GraphicsPipeline*>(pipeline);
  return d3d12->d3d != nullptr;
#else
  return true;
#endif
}

plume::RenderPipeline* EnsurePipeline(GraphicsSystem::DrawPathContext& ctx, const DrawCall& call, plume::RenderShader* vs, plume::RenderShader* ps, uint32_t samples, plume::RenderFormat color_format) {
  const plume::RenderPrimitiveTopology topology = HostTopology(call.primitive);
  PointSpriteVariant* sprite = FindPointSprite(call);
  const bool point_sprites = sprite != nullptr;
  if (sprite) {
    if (!EnsurePointSpriteShaders(ctx, *sprite)) return nullptr;
    vs = sprite->vs.get();
    ps = sprite->ps.get();
  }
  if (UsesWetLensVS(call)) {
    vs = EnsureWetLensVS(ctx);
    if (!vs) return nullptr;
  }

  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
  mix(reinterpret_cast<uintptr_t>(vs));
  mix(reinterpret_cast<uintptr_t>(ps));
  mix(call.layout->key);
  mix(call.layout->dst_stride);
  mix(uint32_t(color_format));
  mix(ctx.depth_format);
  mix(uint32_t(topology));
  mix(call.depth_control);
  mix(call.blend_control);
  mix(call.color_control & 0xF);
  mix(uint64_t(samples) << 8 | (call.alpha_to_mask != 0 ? 1u : 0u));
  mix(call.mode_control & 0x7);
  mix(call.color_mask);
  mix(call.depth_only ? 0x9E3779B97F4A7C15ull : 0ull);
  mix(call.stencil_enable ? call.stencil_ref_mask : 0u);
  // The polygon offset is baked into the PSO, so draws differing only in depth bias must not share one.
  {
    uint32_t o = 0, sc = 0;
    std::memcpy(&o, &call.poly_offset, 4);
    std::memcpy(&sc, &call.poly_offset_scale, 4);
    mix(o);
    mix(sc);
  }

  auto found = g_pipelines.find(key);
  if (found != g_pipelines.end()) {
    return found->second.get();
  }

  std::vector<plume::RenderInputElement> elements;
  elements.reserve(call.layout->elements.size());
  for (const HostElement& e : call.layout->elements) {
    elements.emplace_back(DeclUsageSemantic(e.usage), e.usage_index,
                          DeclUsageLocation(e.usage, e.usage_index), e.format, 0,
                          e.dst_offset);
  }
  const plume::RenderInputSlot slot(0, call.layout->dst_stride);

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = ctx.layout;
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.multisampling.sampleCount = samples;
  desc.geometryShader = point_sprites ? g_pointSpriteGS.get() : nullptr;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements.data();
  desc.inputElementsCount = uint32_t(elements.size());
  desc.primitiveTopology = topology;
  if (call.depth_only) {
    // No pixel shader and no colour attachment, which plume's D3D12 backend already accepts; the framebuffer is depth-only too.
    desc.renderTargetCount = 0;
  } else {
    desc.renderTargetFormat[0] = color_format;
    desc.renderTargetCount = 1;
  }
  desc.depthTargetFormat = plume::RenderFormat(ctx.depth_format);

  desc.depthEnabled = (call.depth_control & state::kDepthTestEnableBit) != 0;
  desc.depthWriteEnabled = (call.depth_control & state::kDepthWriteEnableBit) != 0;
  desc.depthFunction =
      HostCompare((call.depth_control >> state::kDepthFuncShift) & state::kDepthFuncMask);
  if (call.stencil_enable) {
    desc.stencilEnabled = true;
    desc.stencilReference = call.stencil_ref_mask & 0xFF;
    desc.stencilReadMask = (call.stencil_ref_mask >> 8) & 0xFF;
    desc.stencilWriteMask = (call.stencil_ref_mask >> 16) & 0xFF;
    desc.stencilFrontFace = HostStencilFace(call.depth_control, state::kStencilFuncShift);
    desc.stencilBackFace = (call.depth_control & state::kTwoSidedStencilBit) ? HostStencilFace(call.depth_control, state::kStencilFuncShift + 12) : desc.stencilFrontFace;
  }

  // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces; both set (cull everything) maps to FRONT here and is dropped by the replay.
  const bool cull_front = (call.mode_control & state::kCullFrontBit) != 0;
  const bool cull_back = (call.mode_control & state::kCullBackBit) != 0;
  desc.cullMode = cull_front ? plume::RenderCullMode::FRONT
                  : cull_back ? plume::RenderCullMode::BACK
                              : plume::RenderCullMode::NONE;
  if (point_sprites) desc.cullMode = plume::RenderCullMode::NONE;
  // PA_SU_SC_MODE_CNTL bit 2 (FACE): 0 = counter-clockwise is front. -fvk-invert-y puts a SPIR-V triangle where D3D puts it, and both judge winding in top-down framebuffer space, so neither backend flips it.
  const bool front_is_cw = (call.mode_control & state::kFrontFaceCwBit) != 0;
  desc.frontFace = front_is_cw ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;

  // RB_BLENDCONTROL0 is the effective blend on its own: disabling blending writes ONE/ZERO/ADD, and the factor setters return early while it is off.
  plume::RenderBlendDesc blend;
  const uint32_t src = call.blend_control & 0x1F;
  const uint32_t op = (call.blend_control >> 5) & 0x7;
  const uint32_t dst = (call.blend_control >> 8) & 0x1F;
  const uint32_t src_a = (call.blend_control >> 16) & 0x1F;
  const uint32_t op_a = (call.blend_control >> 21) & 0x7;
  const uint32_t dst_a = (call.blend_control >> 24) & 0x1F;
  blend.blendEnabled = !(src == kBlendOne && dst == kBlendZero && op == kBlendOpAdd);
  blend.srcBlend = HostBlend(src);
  blend.dstBlend = HostBlend(dst);
  blend.blendOp = HostBlendOp(op);
  blend.srcBlendAlpha = HostBlendAlpha(src_a);
  blend.dstBlendAlpha = HostBlendAlpha(dst_a);
  blend.blendOpAlpha = HostBlendOp(op_a);
  blend.renderTargetWriteMask = uint8_t(call.color_mask);
  if (!call.depth_only) {
    desc.renderTargetBlend[0] = blend;
  }

  // The guest sets alpha-to-mask on thousands of foliage draws, which on Xenos ran against EDRAM's 4x MSAA; at one sample it would only add a second hard cut, so it follows the real sample count.
  desc.alphaToCoverageEnabled =
      call.alpha_to_mask != 0 && desc.multisampling.sampleCount > plume::RenderSampleCount::COUNT_1;

  // The guest's offset is absolute normalised depth; D3D12's DepthBias is an integer scaled by 2^(zexp - 23) against a float target, so 2^24 converts near the far plane, approximately but enough to stop decals z-fighting.
  if (call.poly_offset != 0.0f || call.poly_offset_scale != 0.0f) {
    const float tune = float(REXCVAR_GET(kameo_gfx_depth_bias_scale));
    desc.depthBias = int32_t(call.poly_offset * 16777216.0f * tune);
    desc.slopeScaledDepthBias = call.poly_offset_scale * tune;
  }

  if (ctx.spirv) {
    desc.specConstants = kGuestSpecConstants;
    desc.specConstantsCount = 1;
  }
  auto pipeline = ctx.device->createGraphicsPipeline(desc);
  if (!PipelineCreated(pipeline.get())) {
    if (++g_pipelineFailures <= 12) {
      KGFX_WARN("pipeline creation failed ({} inputs, stride {}, prim {})",
                  elements.size(), call.layout->dst_stride, call.primitive);
      for (const plume::RenderInputElement& e : elements) {
        KGFX_WARN("  {}{} format {} at {}", e.semanticName, e.semanticIndex,
                    uint32_t(e.format), e.alignedByteOffset);
      }
    }
    g_pipelines.emplace(key, nullptr);
    return nullptr;
  }
#ifdef _WIN32
  // plume's D3D12 backend applies stencilRef in setPipeline but never copies it from the desc.
  if (!ctx.spirv) static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->stencilRef = desc.stencilReference;
#endif
  auto* raw = pipeline.get();
  g_pipelines.emplace(key, std::move(pipeline));
  return raw;
}

size_t PipelineCount() { return g_pipelines.size(); }

void ReleasePipelines() { g_pipelines.clear(); }

}  // namespace kameo::gpu::detail
