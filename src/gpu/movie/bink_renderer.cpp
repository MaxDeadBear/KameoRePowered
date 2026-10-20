#include "gpu/graphics_system.h"

#include <cstring>
#include <iterator>
#include <mutex>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/shaders/builtin_shaders.h"
#include "gpu/textures/texture_codec.h"

// Kameo decodes Bink frames on the CPU into three or four D3DFMT_LIN_L8 planes and blits them with a YUV->RGB shader in Draw_Bink_textures (0x82265558); this reproduces that blit with the game's own matrix.

namespace kameo::gpu {

namespace {

// x,y,z + u,v as a triangle strip, into the swap chain format; depth stays off but its format has to agree with the framebuffers or Vulkan drops the draw.
std::unique_ptr<plume::RenderPipeline> CreateBinkPipeline(plume::RenderDevice* device, plume::RenderPipelineLayout* layout, plume::RenderShader* vs, plume::RenderShader* ps, uint32_t samples) {
  static const plume::RenderInputSlot slot(0, sizeof(float) * 5);
  const plume::RenderInputElement elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, 0),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, sizeof(float) * 3),
  };
  plume::RenderGraphicsPipelineDesc desc;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements;
  desc.inputElementsCount = uint32_t(std::size(elements));
  desc.pipelineLayout = layout;
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.renderTargetFormat[0] = kColorFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  desc.depthTargetFormat = kDepthFormat;
  if (samples > 1) desc.multisampling.sampleCount = samples;
  return device->createGraphicsPipeline(desc);
}

}  // namespace

void GraphicsSystem::DrawPendingBink(plume::RenderCommandList* list, uint32_t samples) {
  bink_in_scene_ = true;
  DrawBink(list, true, samples);
}

void GraphicsSystem::SubmitBinkFrame(const BinkFrame& frame) {
  std::lock_guard<std::mutex> lock(bink_mutex_);
  bink_pending_ = frame;
}

bool GraphicsSystem::EnsureBinkResources() {
  if (bink_pipeline_) {
    return true;
  }
  if (bink_failed_ || !device_) {
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  bink_sampler_ = device_->createSampler(sampler_desc);

  // Bindings are flat across types and must match the HLSL register numbers.
  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 1, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 2, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 3, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 4, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 5, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
  bink_descriptors_ = device_->createDescriptorSet(set_desc);

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = true;
  bink_layout_ = device_->createPipelineLayout(layout_desc);

  const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
  bink_vs_ = CreateBuiltinShader(device_.get(), format, shaders::g_binkVSDxil, shaders::g_binkVSSpirv, "VSMain");
  bink_ps_ = CreateBuiltinShader(device_.get(), format, shaders::g_binkPSDxil, shaders::g_binkPSSpirv, "PSMain");
  if (!bink_vs_ || !bink_ps_) {
    KGFX_ERROR("bink shader creation failed");
    bink_failed_ = true;
    return false;
  }

  bink_pipeline_ = CreateBinkPipeline(device_.get(), bink_layout_.get(), bink_vs_.get(), bink_ps_.get(), 1);
  if (!bink_pipeline_) {
    KGFX_ERROR("bink pipeline creation failed");
    bink_failed_ = true;
    return false;
  }

  for (auto& b : bink_vertices_) b = device_->createBuffer(
      plume::RenderBufferDesc::VertexBuffer(sizeof(float) * 5 * 4, plume::RenderHeapType::UPLOAD));
  bink_constants_ = device_->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(sizeof(float) * 20, plume::RenderBufferFlag::CONSTANT));
  bink_descriptors_->setSampler(4, bink_sampler_.get());
  bink_descriptors_->setBuffer(5, bink_constants_.get(), sizeof(float) * 20);

  KGFX_INFO("bink blit pipeline ready ({})",
              format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  return true;
}

bool GraphicsSystem::UploadBinkPlanes(const BinkFrame& frame) {
  const uint32_t widths[4] = {frame.luma_width, frame.chroma_width, frame.chroma_width,
                              frame.luma_width};
  const uint32_t heights[4] = {frame.luma_height, frame.chroma_height, frame.chroma_height,
                               frame.luma_height};

  // Each plane needs its own region of the staging buffer: the copies only run at submit, so a shared region gives every texture the last plane written.
  uint32_t offsets[4] = {};
  uint32_t total = 0;
  for (int i = 0; i < 4; ++i) {
    if (!frame.planes[i] || widths[i] == 0 || heights[i] == 0) continue;
    offsets[i] = total;
    // A placed footprint has to start on a 512-byte boundary, coarser than the 256-byte rows.
    total = (total + AlignRow(widths[i]) * heights[i] + 511u) & ~511u;
  }
  if (total == 0) return false;

  if (!bink_upload_[frame_slot_] || bink_upload_size_[frame_slot_] < total) {
    bink_upload_[frame_slot_] = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    bink_upload_size_[frame_slot_] = total;
    if (!bink_upload_[frame_slot_]) return false;
  }

  auto* mapped = static_cast<uint8_t*>(bink_upload_[frame_slot_]->map());
  if (!mapped) return false;

  for (int i = 0; i < 4; ++i) {
    if (!frame.planes[i] || widths[i] == 0 || heights[i] == 0) continue;
    const uint32_t dst_pitch = AlignRow(widths[i]);
    uint8_t* dst = mapped + offsets[i];
    for (uint32_t y = 0; y < heights[i]; ++y) {
      std::memcpy(dst + y * dst_pitch, frame.planes[i] + size_t(y) * frame.pitches[i], widths[i]);
    }
  }
  bink_upload_[frame_slot_]->unmap();

  for (int i = 0; i < 4; ++i) {
    if (!frame.planes[i] || widths[i] == 0 || heights[i] == 0) continue;

    if (!bink_textures_[i] || bink_texture_width_[i] != widths[i] ||
        bink_texture_height_[i] != heights[i]) {
      // A replacement movie changes the frame size mid-session, and frames still in flight reference the plane being replaced.
      if (bink_textures_[i]) WaitForAllFrames();
      auto desc = plume::RenderTextureDesc::Texture2D(widths[i], heights[i], 1,
                                                      plume::RenderFormat::R8_UNORM);
      bink_textures_[i] = device_->createTexture(desc);
      if (!bink_textures_[i]) return false;
      bink_views_[i] = bink_textures_[i]->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8_UNORM));
      bink_texture_width_[i] = widths[i];
      bink_texture_height_[i] = heights[i];
      bink_descriptors_->setTexture(uint32_t(i), bink_textures_[i].get(),
                                    plume::RenderTextureLayout::SHADER_READ, bink_views_[i].get());
      if (i == 3) bink_slot3_ = bink_textures_[3].get();
    }

    CurrentCommandList()->barriers(plume::RenderBarrierStage::COPY,
                            plume::RenderTextureBarrier(bink_textures_[i].get(),
                                                        plume::RenderTextureLayout::COPY_DEST));
    auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
        bink_upload_[frame_slot_].get(), plume::RenderFormat::R8_UNORM, widths[i], heights[i], 1,
        AlignRow(widths[i]), offsets[i]);
    auto dst_loc = plume::RenderTextureCopyLocation::Subresource(bink_textures_[i].get(), 0, 0);
    CurrentCommandList()->copyTextureRegion(dst_loc, src, 0, 0, 0, nullptr);
    CurrentCommandList()->barriers(plume::RenderBarrierStage::GRAPHICS,
                            plume::RenderTextureBarrier(bink_textures_[i].get(),
                                                        plume::RenderTextureLayout::SHADER_READ));
  }
  return true;
}

void GraphicsSystem::DrawBink(plume::RenderCommandList* list, bool hold_last, uint32_t samples) {
  // Consume the pending frame rather than copy it, so a frame is drawn only when the guest produced one and a stopped video does not stay frozen under the menu.
  BinkFrame frame;
  bool fresh = false;
  uint32_t held = 0;
  {
    std::lock_guard<std::mutex> lock(bink_mutex_);
    if (bink_pending_.valid) {
      bink_last_ = bink_pending_;
      bink_pending_ = BinkFrame{};
      bink_held_ = 0;
      fresh = true;
    } else if (hold_last) {
      ++bink_held_;
    }
    frame = bink_last_;
    held = bink_held_;
  }
  // A present the guest produced no frame for used to draw no video at all, which is the FMV flicker. The last frame is redrawn instead, but only for a few presents, so a stopped video does not stay frozen under the menu.
  if (!fresh && (!hold_last || held > kBinkHoldPresents)) {
    return;
  }
  if (!frame.valid || !EnsureBinkResources()) {
    return;
  }

  // Redrawing needs no upload: the planes still hold this frame and were left in SHADER_READ.
  if (fresh && !UploadBinkPlanes(frame)) {
    return;
  }

  // A missing alpha plane points its slot at the luma texture so it is always bound (Vulkan rejects the draw otherwise); after the upload, which creates it, and only on change, since Vulkan forbids rewriting a descriptor in flight.
  if (!frame.planes[3] && bink_textures_[0] && bink_slot3_ != bink_textures_[0].get()) {
    WaitForAllFrames();
    bink_descriptors_->setTexture(3, bink_textures_[0].get(),
                                  plume::RenderTextureLayout::SHADER_READ, bink_views_[0].get());
    bink_slot3_ = bink_textures_[0].get();
  }

  struct Constants {
    float yuv_to_rgb[16];
    uint32_t has_alpha;
    uint32_t padding[3];
  } constants{};
  std::memcpy(constants.yuv_to_rgb, frame.yuv_to_rgb, sizeof(constants.yuv_to_rgb));
  constants.has_alpha = frame.has_alpha ? 1u : 0u;
  // Written only when the colour space changes, once per video, because the descriptor set binding it does not rotate.
  if (bink_constants_shadow_.size() != sizeof(constants) ||
      std::memcmp(bink_constants_shadow_.data(), &constants, sizeof(constants)) != 0) {
    WaitForAllFrames();
    if (void* mapped = bink_constants_->map()) {
      std::memcpy(mapped, &constants, sizeof(constants));
      bink_constants_->unmap();
      const auto* bytes = reinterpret_cast<const uint8_t*>(&constants);
      bink_constants_shadow_.assign(bytes, bytes + sizeof(constants));
    }
  }

  if (void* mapped = bink_vertices_[frame_slot_]->map()) {
    std::memcpy(mapped, frame.vertices, sizeof(frame.vertices));
    bink_vertices_[frame_slot_]->unmap();
  }

  const plume::RenderVertexBufferView view(bink_vertices_[frame_slot_].get(), sizeof(frame.vertices));
  const plume::RenderInputSlot slot(0, sizeof(float) * 5);
  plume::RenderPipeline* pipeline = bink_pipeline_.get();
  if (samples > 1) {
    auto& msaa_pipeline = bink_pipelines_msaa_[samples];
    if (!msaa_pipeline && bink_vs_ && bink_ps_) msaa_pipeline = CreateBinkPipeline(device_.get(), bink_layout_.get(), bink_vs_.get(), bink_ps_.get(), samples);
    pipeline = msaa_pipeline.get();
  }
  if (!pipeline) return;
  list->setPipeline(pipeline);
  list->setGraphicsPipelineLayout(bink_layout_.get());
  list->setGraphicsDescriptorSet(bink_descriptors_.get(), 0);
  list->setVertexBuffers(0, &view, 1, &slot);
  list->drawInstanced(4, 1, 0, 0);
}

}  // namespace kameo::gpu
