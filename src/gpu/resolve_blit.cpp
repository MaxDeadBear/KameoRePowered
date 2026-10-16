#include "gpu/resolve_blit.h"

#include <iterator>
#include <memory>
#include <vector>

#include "gpu/log.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/shaders/builtin_shaders.h"
#include "gpu/surface_pool.h"

namespace kameo::gpu::detail {
namespace {

// A resolve out of the scene reads a framebuffer scaled to the window, which CopyTextureRegion can only crop, so it is drawn, filtered, into the guest-sized target.
struct ResolveBlit {
  std::unique_ptr<plume::RenderSampler> sampler;
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  // The same blit into a 16-bit float destination, for copies of wide-format passes.
  std::unique_ptr<plume::RenderPipeline> pipeline_wide;
  std::vector<std::unique_ptr<plume::RenderDescriptorSet>> sets;
  std::vector<std::unique_ptr<plume::RenderBuffer>> constants;
  uint32_t next = 0;   // next free slot in this frame's range
  uint32_t limit = 0;  // one past the end of it
  bool failed = false;
};
ResolveBlit g_blit;

// Draws the guest frame's rectangle of the scene depth into a single-sample R32F copy, since the API resolves colour but not depth and a whole-resource copy would include the pillarbox bars.
// Every depth copy reads its own origin, so each takes a slot of constants, descriptors and view; imposterModelRender resolves dozens of scratch-pad slots a frame.
constexpr uint32_t kDepthResolveSlotsPerFrame = 128;
struct DepthResolvePass {
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  std::unique_ptr<plume::RenderPipeline> pipeline_single;
  std::vector<std::unique_ptr<plume::RenderDescriptorSet>> sets;
  std::vector<std::unique_ptr<plume::RenderBuffer>> constants;
  std::vector<std::unique_ptr<plume::RenderTextureView>> views;
  uint32_t next = 0;
  uint32_t limit = 0;
  bool failed = false;
};
DepthResolvePass g_depthResolve;

// Descriptor sets and their constants are read when the list executes, so each frame in flight owns its own slice; a busy frame resolves ~38 times.
constexpr uint32_t kBlitSlots = 256;
constexpr uint32_t kBlitSlotsPerFrame = kBlitSlots / kFramesInFlight;

// A descriptor set is read at execute time, so every blit in a frame needs its own.
bool EnsureResolveBlit(GraphicsSystem::DrawPathContext& ctx) {
  if (g_blit.failed) {
    return false;
  }
  if (g_blit.pipeline) {
    return true;
  }
  g_blit.sampler = ctx.device->createSampler(plume::RenderSamplerDesc());
  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = false;
  g_blit.layout = ctx.device->createPipelineLayout(layout_desc);

  const plume::RenderShaderFormat fmt = ctx.spirv ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL;
  std::unique_ptr<plume::RenderShader> vs;
  std::unique_ptr<plume::RenderShader> ps;
  if (ctx.spirv) {
    vs = ctx.device->createShader(shaders::g_blitVSSpirv, sizeof(shaders::g_blitVSSpirv), "VSMain", fmt);
    ps = ctx.device->createShader(shaders::g_blitPSSpirv, sizeof(shaders::g_blitPSSpirv), "PSMain", fmt);
  } else {
    vs = ctx.device->createShader(shaders::g_blitVSDxil, sizeof(shaders::g_blitVSDxil), "VSMain", fmt);
    ps = ctx.device->createShader(shaders::g_blitPSDxil, sizeof(shaders::g_blitPSDxil), "PSMain", fmt);
  }
  if (!g_blit.layout || !vs || !ps) {
    KGFX_ERROR("resolve blit shader/layout creation failed");
    g_blit.failed = true;
    return false;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_blit.layout.get();
  desc.vertexShader = vs.get();
  desc.pixelShader = ps.get();
  desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.cullMode = plume::RenderCullMode::NONE;
  g_blit.pipeline = ctx.device->createGraphicsPipeline(desc);
  if (!PipelineCreated(g_blit.pipeline.get())) {
    KGFX_ERROR("resolve blit pipeline REJECTED by the backend");
    g_blit.failed = true;
    return false;
  }
  desc.renderTargetFormat[0] = kWideColorFormat;
  g_blit.pipeline_wide = ctx.device->createGraphicsPipeline(desc);
  if (!PipelineCreated(g_blit.pipeline_wide.get())) {
    KGFX_WARN("wide resolve blit pipeline rejected; wide copies stay unscaled");
    g_blit.pipeline_wide.reset();
  }

  g_blit.sets.resize(kBlitSlots);
  g_blit.constants.resize(kBlitSlots);
  for (uint32_t i = 0; i < kBlitSlots; ++i) {
    g_blit.sets[i] = ctx.device->createDescriptorSet(set_desc);
    g_blit.constants[i] = ctx.device->createBuffer(
        plume::RenderBufferDesc::UploadBuffer(16, plume::RenderBufferFlag::CONSTANT));
    if (!g_blit.sets[i] || !g_blit.constants[i]) {
      KGFX_ERROR("resolve blit slot {} allocation failed", i);
      g_blit.failed = true;
      return false;
    }
    g_blit.sets[i]->setSampler(1, g_blit.sampler.get());
    g_blit.sets[i]->setBuffer(2, g_blit.constants[i].get(), 16);
  }
  KGFX_INFO("resolve blit ready ({} slots, {})", kBlitSlots, ctx.spirv ? "spirv" : "dxil");
  return true;
}

}  // namespace

void BeginResolveBlitFrame(uint32_t frame_slot) {
  g_blit.next = frame_slot * kBlitSlotsPerFrame;
  g_depthResolve.next = frame_slot * kDepthResolveSlotsPerFrame;
  g_depthResolve.limit = g_depthResolve.next + kDepthResolveSlotsPerFrame;
  g_blit.limit = g_blit.next + kBlitSlotsPerFrame;
}

bool DrawDepthResolve(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, plume::RenderTexture* depth, CachedTexture& dest, int32_t origin_x, int32_t origin_y, bool multisampled) {
  if (g_depthResolve.failed || !dest.framebuffer) return false;
  if (!g_depthResolve.pipeline) {
    const plume::RenderDescriptorRange ranges[] = {
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 1, 1),
    };
    plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
    plume::RenderPipelineLayoutDesc layout_desc;
    layout_desc.descriptorSetDescs = &set_desc;
    layout_desc.descriptorSetDescsCount = 1;
    layout_desc.allowInputLayout = false;
    g_depthResolve.layout = ctx.device->createPipelineLayout(layout_desc);
    const uint32_t slots = kDepthResolveSlotsPerFrame * kFramesInFlight;
    g_depthResolve.sets.resize(slots);
    g_depthResolve.constants.resize(slots);
    g_depthResolve.views.resize(slots);
    for (uint32_t i = 0; i < slots; ++i) {
      g_depthResolve.sets[i] = ctx.device->createDescriptorSet(set_desc);
      g_depthResolve.constants[i] = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(16, plume::RenderBufferFlag::CONSTANT));
      if (!g_depthResolve.sets[i] || !g_depthResolve.constants[i]) {
        KGFX_ERROR("depth resolve slot creation failed");
        g_depthResolve.failed = true;
        return false;
      }
      g_depthResolve.sets[i]->setBuffer(1, g_depthResolve.constants[i].get(), 16);
    }
    const plume::RenderShaderFormat fmt = ctx.spirv ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL;
    auto vs = ctx.spirv ? ctx.device->createShader(shaders::g_depthResolveVSSpirv, sizeof(shaders::g_depthResolveVSSpirv), "VSMain", fmt) : ctx.device->createShader(shaders::g_depthResolveVSDxil, sizeof(shaders::g_depthResolveVSDxil), "VSMain", fmt);
    auto ps = ctx.spirv ? ctx.device->createShader(shaders::g_depthResolvePSSpirv, sizeof(shaders::g_depthResolvePSSpirv), "PSMain", fmt) : ctx.device->createShader(shaders::g_depthResolvePSDxil, sizeof(shaders::g_depthResolvePSDxil), "PSMain", fmt);
    auto ps_single = ctx.spirv ? ctx.device->createShader(shaders::g_depthResolveSinglePSSpirv, sizeof(shaders::g_depthResolveSinglePSSpirv), "PSSingle", fmt) : ctx.device->createShader(shaders::g_depthResolveSinglePSDxil, sizeof(shaders::g_depthResolveSinglePSDxil), "PSSingle", fmt);
    if (!g_depthResolve.layout || !vs || !ps || !ps_single) {
      KGFX_ERROR("depth resolve shader/layout creation failed");
      g_depthResolve.failed = true;
      return false;
    }
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_depthResolve.layout.get();
    desc.vertexShader = vs.get();
    desc.pixelShader = ps.get();
    desc.renderTargetFormat[0] = plume::RenderFormat::R32_FLOAT;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.depthEnabled = false;
    desc.depthWriteEnabled = false;
    desc.cullMode = plume::RenderCullMode::NONE;
    g_depthResolve.pipeline = ctx.device->createGraphicsPipeline(desc);
    desc.pixelShader = ps_single.get();
    g_depthResolve.pipeline_single = ctx.device->createGraphicsPipeline(desc);
    if (!PipelineCreated(g_depthResolve.pipeline.get()) || !PipelineCreated(g_depthResolve.pipeline_single.get())) {
      KGFX_ERROR("depth resolve pipeline REJECTED by the backend");
      g_depthResolve.failed = true;
      return false;
    }
  }
  if (g_depthResolve.next >= g_depthResolve.limit) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      KGFX_WARN("more than {} depth resolves in one frame; the rest are skipped", kDepthResolveSlotsPerFrame);
    }
    return false;
  }
  const uint32_t slot = g_depthResolve.next++;
  if (int32_t* mapped = static_cast<int32_t*>(g_depthResolve.constants[slot]->map())) {
    mapped[0] = origin_x;
    mapped[1] = origin_y;
    mapped[2] = 0;
    mapped[3] = 0;
    g_depthResolve.constants[slot]->unmap();
  }
  // A view per copy: a recreated depth buffer can reuse the old one's address, and the slot is not reused until its frame has retired.
  g_depthResolve.views[slot] = depth->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat(ctx.depth_format)));
  g_depthResolve.sets[slot]->setTexture(0, depth, plume::RenderTextureLayout::DEPTH_READ, g_depthResolve.views[slot].get());
  const plume::RenderTextureBarrier to_draw[] = {
      plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_READ),
      plume::RenderTextureBarrier(dest.texture.get(), plume::RenderTextureLayout::COLOR_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_draw, uint32_t(std::size(to_draw)));
  list->setFramebuffer(dest.framebuffer.get());
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height)));
  list->setScissors(plume::RenderRect(0, 0, dest.width, dest.height));
  list->setPipeline(multisampled ? g_depthResolve.pipeline.get() : g_depthResolve.pipeline_single.get());
  list->setGraphicsPipelineLayout(g_depthResolve.layout.get());
  list->setGraphicsDescriptorSet(g_depthResolve.sets[slot].get(), 0);
  list->drawInstanced(3, 1, 0, 0);
  return true;
}

bool BlitScaledResolve(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, plume::RenderTexture* source, CachedTexture& dest, const float* uv, int32_t dest_x, int32_t dest_y, uint32_t dest_width, uint32_t dest_height) {
  if (!EnsureResolveBlit(ctx) || !dest.framebuffer) {
    return false;
  }
  plume::RenderPipeline* pipeline = dest.format == kWideColorFormat ? g_blit.pipeline_wide.get() : g_blit.pipeline.get();
  if (!pipeline) return false;
  if (g_blit.next >= g_blit.limit) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      KGFX_WARN("more than {} scaling resolves in one frame; the rest CROP",
                  kBlitSlotsPerFrame);
    }
    return false;
  }
  const uint32_t slot = g_blit.next++;
  if (float* mapped = static_cast<float*>(g_blit.constants[slot]->map())) {
    mapped[0] = uv[0];
    mapped[1] = uv[1];
    mapped[2] = uv[2];
    mapped[3] = uv[3];
    g_blit.constants[slot]->unmap();
  }
  g_blit.sets[slot]->setTexture(0, source, plume::RenderTextureLayout::SHADER_READ, nullptr);

  const plume::RenderTextureBarrier to_blit[] = {
      plume::RenderTextureBarrier(source, plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(dest.texture.get(), plume::RenderTextureLayout::COLOR_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_blit, uint32_t(std::size(to_blit)));
  list->setFramebuffer(dest.framebuffer.get());
  list->setViewports(plume::RenderViewport(float(dest_x), float(dest_y), float(dest_width), float(dest_height)));
  list->setScissors(plume::RenderRect(dest_x, dest_y, dest_x + dest_width, dest_y + dest_height));
  list->setPipeline(pipeline);
  list->setGraphicsPipelineLayout(g_blit.layout.get());
  list->setGraphicsDescriptorSet(g_blit.sets[slot].get(), 0);
  list->drawInstanced(3, 1, 0, 0);
  return true;
}

void ReleaseResolveBlits() {
  g_blit.sets.clear();
  g_blit.constants.clear();
  g_blit.pipeline.reset();
  g_blit.pipeline_wide.reset();
  g_depthResolve = DepthResolvePass();
  g_blit.layout.reset();
  g_blit.sampler.reset();
  g_blit.next = 0;
  g_blit.limit = 0;
  g_blit.failed = false;
}

}  // namespace kameo::gpu::detail
