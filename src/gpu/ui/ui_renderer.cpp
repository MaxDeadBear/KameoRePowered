#include "gpu/graphics_system.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/shaders/builtin_shaders.h"
#include "gpu/textures/texture_codec.h"

namespace kameo::gpu {

void GraphicsSystem::RunUiCallback() {
  const uint32_t width = ui_width_.load(std::memory_order_relaxed);
  const uint32_t height = ui_height_.load(std::memory_order_relaxed);
  if (width == 0 || height == 0) return;
  std::function<void(uint32_t, uint32_t)> callback;
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    callback = ui_frame_callback_;
  }
  if (callback) callback(width, height);
}

// The ImGui pipeline: clip-space vertices, an RGBA texture per draw, alpha blended over the finished frame.
bool GraphicsSystem::EnsureUiPipeline() {
  if (ui_pipeline_) {
    return true;
  }
  if (ui_pipeline_failed_ || !device_) {
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  ui_sampler_ = device_->createSampler(sampler_desc);

  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = true;
  ui_layout_ = device_->createPipelineLayout(layout_desc);

  const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
  std::unique_ptr<plume::RenderShader> vs = CreateBuiltinShader(device_.get(), format, shaders::g_overlayVSDxil, shaders::g_overlayVSSpirv, "VSMain");
  if (!vs) {
    KGFX_ERROR("overlay shader creation failed");
    ui_pipeline_failed_ = true;
    return false;
  }
  std::unique_ptr<plume::RenderShader> ps = CreateBuiltinShader(device_.get(), format, shaders::g_overlayRgbaPSDxil, shaders::g_overlayRgbaPSSpirv, "PSMainRGBA");
  if (!ps) {
    KGFX_ERROR("overlay RGBA shader creation failed");
    ui_pipeline_failed_ = true;
    return false;
  }

  static const plume::RenderInputSlot slot(0, sizeof(UiVertex));
  const plume::RenderInputElement elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0, 0),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0,
                                sizeof(float) * 2),
      plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R32G32B32A32_FLOAT, 0,
                                sizeof(float) * 4),
  };

  plume::RenderGraphicsPipelineDesc pipeline_desc;
  pipeline_desc.inputSlots = &slot;
  pipeline_desc.inputSlotsCount = 1;
  pipeline_desc.inputElements = elements;
  pipeline_desc.inputElementsCount = uint32_t(std::size(elements));
  pipeline_desc.pipelineLayout = ui_layout_.get();
  pipeline_desc.vertexShader = vs.get();
  pipeline_desc.pixelShader = ps.get();
  pipeline_desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  // ImGui text is antialiased coverage, so it has to composite rather than overwrite.
  pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  pipeline_desc.renderTargetCount = 1;
  pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  // The swap chain framebuffers carry depth, so the format has to agree even with depth off or Vulkan drops the draw.
  pipeline_desc.depthTargetFormat = kDepthFormat;
  ui_pipeline_ = device_->createGraphicsPipeline(pipeline_desc);
  if (!ui_pipeline_) {
    KGFX_ERROR("overlay RGBA pipeline creation failed");
    ui_pipeline_failed_ = true;
    return false;
  }

  KGFX_INFO("overlay pipeline ready ({})",
              format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  return true;
}

uint32_t GraphicsSystem::CreateUiTexture(uint32_t width, uint32_t height, const uint8_t* rgba) {
  if (!width || !height || !rgba) {
    return 0;
  }
  UiTexture entry;
  entry.width = width;
  entry.height = height;
  entry.pixels.assign(rgba, rgba + size_t(width) * height * 4);
  std::lock_guard<std::mutex> lock(ui_mutex_);
  const uint32_t id = ui_next_texture_++;
  ui_textures_.emplace(id, std::move(entry));
  return id;
}

void GraphicsSystem::SetUiFrameCallback(std::function<void(uint32_t, uint32_t)> callback) {
  std::lock_guard<std::mutex> lock(ui_mutex_);
  ui_frame_callback_ = std::move(callback);
}

void GraphicsSystem::SubmitUi(UiFrame&& frame) {
  std::lock_guard<std::mutex> lock(ui_mutex_);
  ui_pending_ = std::move(frame);
  ui_pending_fresh_ = true;
}

void GraphicsSystem::DrawUi(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height) {
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    if (ui_pending_fresh_) {
      ui_current_ = std::move(ui_pending_);
      ui_pending_ = UiFrame();
      ui_pending_fresh_ = false;
    }
  }
  const UiFrame& frame = ui_current_;
  if (frame.draws.empty() || frame.vertices.empty() || !EnsureUiPipeline()) {
    return;
  }
  // The scene leaves whatever target and viewport its last draw used, which squeezed the interface into a guest viewport when the scene rendered straight into the window.
  list->setFramebuffer(framebuffer);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));

  // Upload textures registered since the last frame here, since CreateUiTexture runs on the UI thread with no list to record into.
  // Each texture gets its own region of the upload buffer: the copies run at submit, so a shared region handed every copy the last texture's bytes and blanked the font atlas.
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    constexpr uint32_t kPlacementAlign = 512;
    struct PendingUpload {
      uint32_t id = 0;
      uint32_t offset = 0;
      uint32_t pitch = 0;
    };
    std::vector<PendingUpload> pending;
    uint32_t staged = 0;
    for (auto& [id, tex] : ui_textures_) {
      if (tex.pixels.empty()) {
        continue;
      }
      auto desc = plume::RenderTextureDesc::Texture2D(
          tex.width, tex.height, 1, plume::RenderFormat::R8G8B8A8_UNORM);
      tex.texture = device_->createTexture(desc);
      if (!tex.texture) {
        tex.pixels.clear();
        continue;
      }
      tex.view = tex.texture->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
      plume::RenderDescriptorRange ranges[] = {
          plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
          plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
      };
      plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
      tex.descriptors = device_->createDescriptorSet(set_desc);
      if (!tex.descriptors) {
        tex.pixels.clear();
        continue;
      }
      tex.descriptors->setTexture(0, tex.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                                  tex.view.get());
      tex.descriptors->setSampler(1, ui_sampler_.get());
      const uint32_t pitch = AlignRow(tex.width * 4);
      staged = (staged + kPlacementAlign - 1) & ~(kPlacementAlign - 1);
      pending.push_back({id, staged, pitch});
      staged += pitch * tex.height;
    }

    if (!pending.empty()) {
      if (!ui_upload_[frame_slot_] || ui_upload_size_[frame_slot_] < staged) {
        ui_upload_[frame_slot_] =
            device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(staged));
        ui_upload_size_[frame_slot_] = staged;
      }
      uint8_t* mapped = ui_upload_[frame_slot_]
                            ? static_cast<uint8_t*>(ui_upload_[frame_slot_]->map())
                            : nullptr;
      if (mapped) {
        for (const PendingUpload& up : pending) {
          const UiTexture& tex = ui_textures_[up.id];
          for (uint32_t y = 0; y < tex.height; ++y) {
            std::memcpy(mapped + up.offset + size_t(y) * up.pitch,
                        tex.pixels.data() + size_t(y) * tex.width * 4, size_t(tex.width) * 4);
          }
        }
        ui_upload_[frame_slot_]->unmap();
        for (const PendingUpload& up : pending) {
          const UiTexture& tex = ui_textures_[up.id];
          list->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(tex.texture.get(),
                                                     plume::RenderTextureLayout::COPY_DEST));
          auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
              ui_upload_[frame_slot_].get(), plume::RenderFormat::R8G8B8A8_UNORM, tex.width,
              tex.height, 1, up.pitch / 4, up.offset);
          auto dst_loc = plume::RenderTextureCopyLocation::Subresource(tex.texture.get(), 0, 0);
          list->copyTextureRegion(dst_loc, src, 0, 0, 0, nullptr);
          list->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(tex.texture.get(),
                                                     plume::RenderTextureLayout::SHADER_READ));
          KGFX_INFO("ui texture {} {}x{} uploaded at +{}", up.id, tex.width, tex.height, up.offset);
        }
      }
      for (const PendingUpload& up : pending) {
        UiTexture& tex = ui_textures_[up.id];
        tex.pixels.clear();
        tex.pixels.shrink_to_fit();
      }
    }
  }

  const uint32_t count = uint32_t(frame.vertices.size());
  const uint32_t bytes = count * sizeof(UiVertex);
  if (!ui_vertices_[frame_slot_] || ui_vertex_capacity_[frame_slot_] < count) {
    ui_vertices_[frame_slot_] = device_->createBuffer(
        plume::RenderBufferDesc::VertexBuffer(bytes, plume::RenderHeapType::UPLOAD));
    ui_vertex_capacity_[frame_slot_] = count;
    if (!ui_vertices_[frame_slot_]) return;
  }
  if (auto* mapped = static_cast<uint8_t*>(ui_vertices_[frame_slot_]->map())) {
    std::memcpy(mapped, frame.vertices.data(), bytes);
    ui_vertices_[frame_slot_]->unmap();
  } else {
    return;
  }

  const plume::RenderVertexBufferView view(ui_vertices_[frame_slot_].get(), bytes);
  const plume::RenderInputSlot slot(0, sizeof(UiVertex));
  list->setPipeline(ui_pipeline_.get());
  list->setGraphicsPipelineLayout(ui_layout_.get());
  list->setVertexBuffers(0, &view, 1, &slot);

  std::lock_guard<std::mutex> lock(ui_mutex_);
  for (const UiDraw& draw : frame.draws) {
    auto found = ui_textures_.find(draw.texture_id);
    if (found == ui_textures_.end() || !found->second.descriptors) {
      continue;
    }
    if (draw.scissor) {
      const int32_t x = std::clamp(draw.scissor_x, 0, int32_t(width));
      const int32_t y = std::clamp(draw.scissor_y, 0, int32_t(height));
      const int32_t w = std::clamp(draw.scissor_w, 0, int32_t(width) - x);
      const int32_t h = std::clamp(draw.scissor_h, 0, int32_t(height) - y);
      if (w <= 0 || h <= 0) continue;
      list->setScissors(plume::RenderRect(x, y, x + w, y + h));
    } else {
      list->setScissors(plume::RenderRect(0, 0, width, height));
    }
    list->setGraphicsDescriptorSet(found->second.descriptors.get(), 0);
    list->drawInstanced(draw.vertex_count, 1, draw.first_vertex, 0);
  }
  list->setScissors(plume::RenderRect(0, 0, width, height));
}

}  // namespace kameo::gpu
