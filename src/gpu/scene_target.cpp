#include "gpu/graphics_system.h"

#include <algorithm>
#include <iterator>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/shaders/builtin_shaders.h"

namespace kameo::gpu {

// The guest renders a 16:9 image; the width follows the WINDOW's aspect so an ultrawide gets a wider target and the scene is pillarboxed inside it exactly as it is at window size.
void GraphicsSystem::SceneRenderSize(uint32_t window_width, uint32_t window_height, uint32_t* out_width, uint32_t* out_height) const {
  *out_width = window_width;
  *out_height = window_height;
  const int32_t wanted = REXCVAR_GET(kameo_gfx_render_height);
  if (wanted <= 0 || window_width == 0 || window_height == 0) return;
  const uint32_t height = uint32_t(std::clamp(wanted, 240, 4320));
  *out_height = height;
  if (const int32_t wanted_width = REXCVAR_GET(kameo_gfx_render_width); wanted_width > 0) {
    *out_width = uint32_t(std::clamp(wanted_width, 320, 8192));
    return;
  }
  const uint64_t width = (uint64_t(height) * window_width + window_height / 2) / window_height;
  *out_width = uint32_t(std::clamp<uint64_t>(width & ~1ull, 320, 8192));
}

void GraphicsSystem::SceneRectInWindow(uint32_t window_width, uint32_t window_height, float* x, float* y, float* width, float* height, uint32_t* scene_width, uint32_t* scene_height) const {
  *scene_width = window_width;
  *scene_height = window_height;
  SceneRenderSize(window_width, window_height, scene_width, scene_height);
  const float scale = (*scene_width && *scene_height) ? std::min(float(window_width) / float(*scene_width), float(window_height) / float(*scene_height)) : 1.0f;
  *width = float(*scene_width) * scale;
  *height = float(*scene_height) * scale;
  *x = (float(window_width) - *width) * 0.5f;
  *y = (float(window_height) - *height) * 0.5f;
}

uint32_t GraphicsSystem::SceneSampleCount() const {
  const int32_t wanted = REXCVAR_GET(kameo_gfx_msaa);
  if (wanted <= 1 || !device_) return 1;
  const plume::RenderSampleCounts supported = device_->getSampleCountsSupported(kColorFormat) & device_->getSampleCountsSupported(kDepthFormat);
  uint32_t count = wanted >= 8 ? 8 : wanted >= 4 ? 4 : 2;
  while (count > 1 && (supported & count) == 0) count >>= 1;
  return count;
}

bool GraphicsSystem::EnsureSceneTarget(uint32_t width, uint32_t height, uint32_t samples) {
  if (scene_target_failed_ || !device_) return false;
  if (scene_framebuffer_ && scene_width_ == width && scene_height_ == height && scene_samples_ == samples) return true;
  // A frame still in flight can be reading the old pair.
  WaitForAllFrames();
  scene_framebuffer_.reset();
  scene_color_view_.reset();
  scene_color_.reset();
  scene_depth_view_.reset();
  scene_depth_.reset();
  scene_resolved_view_.reset();
  scene_resolved_.reset();
  const plume::RenderMultisampling multisampling(samples);
  scene_color_ = device_->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kColorFormat, multisampling));
  scene_depth_ = device_->createTexture(plume::RenderTextureDesc::DepthTarget(width, height, kDepthFormat, multisampling));
  if (samples > 1) scene_resolved_ = device_->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kColorFormat));
  if (!scene_color_ || !scene_depth_ || (samples > 1 && !scene_resolved_)) {
    KGFX_ERROR("scene target {}x{} at {} samples creation failed; rendering at the window's size", width, height, samples);
    scene_target_failed_ = true;
    return false;
  }
  scene_color_view_ = scene_color_->createTextureView(plume::RenderTextureViewDesc::Texture2D(kColorFormat));
  if (scene_resolved_) scene_resolved_view_ = scene_resolved_->createTextureView(plume::RenderTextureViewDesc::Texture2D(kColorFormat));
  scene_depth_view_ = scene_depth_->createTextureView(plume::RenderTextureViewDesc::Texture2D(kDepthFormat));
  const plume::RenderTexture* color = scene_color_.get();
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = &color;
  desc.colorAttachmentsCount = 1;
  desc.depthAttachment = scene_depth_.get();
  desc.depthAttachmentView = scene_depth_view_.get();
  scene_framebuffer_ = device_->createFramebuffer(desc);
  if (!scene_framebuffer_) {
    KGFX_ERROR("scene framebuffer {}x{} creation failed; rendering at the window's size", width, height);
    scene_target_failed_ = true;
    return false;
  }
  scene_width_ = width;
  scene_height_ = height;
  scene_samples_ = samples;
  ++scene_target_generation_;
  KGFX_INFO("rendering the scene at {}x{} with {} sample(s), scaled to the window at present", width, height, samples);
  return true;
}

bool GraphicsSystem::EnsureUpscaleResources() {
  if (upscale_pipeline_) return true;
  if (upscale_failed_ || !device_) return false;
  upscale_sampler_ = device_->createSampler(plume::RenderSamplerDesc());
  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    upscale_descriptors_[i] = device_->createDescriptorSet(set_desc);
    if (!upscale_descriptors_[i]) {
      KGFX_ERROR("scene upscale descriptor set creation failed");
      upscale_failed_ = true;
      return false;
    }
  }
  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = false;
  upscale_layout_ = device_->createPipelineLayout(layout_desc);
  const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
  std::unique_ptr<plume::RenderShader> vs = CreateBuiltinShader(device_.get(), format, shaders::g_blitVSDxil, shaders::g_blitVSSpirv, "VSMain");
  std::unique_ptr<plume::RenderShader> ps = CreateBuiltinShader(device_.get(), format, shaders::g_blitPSDxil, shaders::g_blitPSSpirv, "PSMain");
  if (!vs || !ps) {
    KGFX_ERROR("scene upscale shader creation failed");
    upscale_failed_ = true;
    return false;
  }
  upscale_pipeline_ = CreateFullscreenPipeline(device_.get(), upscale_layout_.get(), vs.get(), ps.get());
  if (!upscale_pipeline_) {
    KGFX_ERROR("scene upscale pipeline creation failed");
    upscale_failed_ = true;
    return false;
  }
  upscale_constants_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(16, plume::RenderBufferFlag::CONSTANT));
  if (float* mapped = static_cast<float*>(upscale_constants_->map())) {
    mapped[0] = 0.0f;
    mapped[1] = 0.0f;
    mapped[2] = 1.0f;
    mapped[3] = 1.0f;
    upscale_constants_->unmap();
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    upscale_descriptors_[i]->setSampler(1, upscale_sampler_.get());
    upscale_descriptors_[i]->setBuffer(2, upscale_constants_.get(), 16);
  }
  KGFX_INFO("scene upscale ready ({})", format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  return true;
}

// Resolves a multisampled scene, then scales it onto the swap chain image, letterboxed to its own aspect.
void GraphicsSystem::UpscaleScene(plume::RenderCommandList* list, plume::RenderTexture* scene_texture, plume::RenderTexture* texture, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height) {
  plume::RenderTexture* upscale_source = scene_texture;
  plume::RenderTextureView* upscale_view = scene_color_view_.get();
  if (active_scene_samples_ > 1 && scene_resolved_) {
    const plume::RenderTextureBarrier to_resolve[] = {
        plume::RenderTextureBarrier(scene_texture, plume::RenderTextureLayout::RESOLVE_SOURCE),
        plume::RenderTextureBarrier(scene_resolved_.get(), plume::RenderTextureLayout::RESOLVE_DEST),
    };
    list->barriers(plume::RenderBarrierStage::COPY, to_resolve, uint32_t(std::size(to_resolve)));
    list->resolveTexture(scene_resolved_.get(), scene_texture);
    upscale_source = scene_resolved_.get();
    upscale_view = scene_resolved_view_.get();
  }
  const plume::RenderTextureBarrier to_upscale[] = {
      plume::RenderTextureBarrier(upscale_source, plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(depth_texture_.get(), plume::RenderTextureLayout::DEPTH_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_upscale, uint32_t(std::size(to_upscale)));
  upscale_descriptors_[frame_slot_]->setTexture(0, upscale_source, plume::RenderTextureLayout::SHADER_READ, upscale_view);
  list->setFramebuffer(framebuffer);
  float fit_x = 0.0f;
  float fit_y = 0.0f;
  float fit_width = float(width);
  float fit_height = float(height);
  uint32_t fit_scene_width = 0;
  uint32_t fit_scene_height = 0;
  SceneRectInWindow(width, height, &fit_x, &fit_y, &fit_width, &fit_height, &fit_scene_width, &fit_scene_height);
  if (fit_width < float(width) - 0.5f || fit_height < float(height) - 0.5f) list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  list->setViewports(plume::RenderViewport(fit_x, fit_y, fit_width, fit_height));
  list->setScissors(plume::RenderRect(0, 0, width, height));
  list->setPipeline(upscale_pipeline_.get());
  list->setGraphicsPipelineLayout(upscale_layout_.get());
  list->setGraphicsDescriptorSet(upscale_descriptors_[frame_slot_].get(), 0);
  list->drawInstanced(3, 1, 0, 0);
}

double GraphicsSystem::DisplayAspect() const {
  // The scene's own shape, which is the window's unless a render size of another shape was chosen.
  uint32_t width = ui_width_.load(std::memory_order_relaxed);
  uint32_t height = ui_height_.load(std::memory_order_relaxed);
  if (width && height) SceneRenderSize(width, height, &width, &height);
  return (width && height) ? double(width) / double(height) : 16.0 / 9.0;
}

}  // namespace kameo::gpu
