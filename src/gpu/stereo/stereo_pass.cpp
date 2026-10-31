// Stereoscopic 3D on the render thread: the first eye is set aside between the two replays, and the composite puts both onto the swap chain. The per-eye shift itself is applied by the recompiled vertex shaders from the shared constants (see stereo.h).

#include <algorithm>
#include <iterator>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/graphics_system.h"
#include "gpu/frame_profile.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/resource_check.h"
#include "gpu/scene_resolve_state.h"
#include "gpu/shaders/builtin_shaders.h"
#include "gpu/stereo/stereo.h"

namespace kameo::gpu {

// Called from Shutdown with the other GPU objects, since none may outlive the device.
void GraphicsSystem::ReleaseStereoResources() {
  for (auto& descriptors : stereo_descriptors_) descriptors.reset();
  for (auto& buffer : stereo_constants_) buffer.reset();
  stereo_pipeline_.reset();
  stereo_layout_.reset();
  stereo_sampler_.reset();
  stereo_eye_view_.reset();
  stereo_eye_.reset();
}

bool GraphicsSystem::EnsureStereoResources(uint32_t width, uint32_t height) {
  if (stereo_failed_ || !device_) return false;
  if (!stereo_eye_ || stereo_width_ != width || stereo_height_ != height) {
    // A frame still in flight can be reading the old texture.
    WaitForAllFrames();
    stereo_eye_view_.reset();
    stereo_eye_.reset();
    // Single-sampled whatever the scene is: a multisampled scene is resolved into it.
    stereo_eye_ = detail::CreateTextureChecked(device_.get(), plume::RenderTextureDesc::ColorTarget(width, height, kColorFormat), "stereo eye");
    if (!stereo_eye_) {
      KGFX_ERROR("stereo eye texture {}x{} creation failed; 3D is off", width, height);
      stereo_failed_ = true;
      return false;
    }
    stereo_eye_view_ = stereo_eye_->createTextureView(plume::RenderTextureViewDesc::Texture2D(kColorFormat));
    stereo_width_ = width;
    stereo_height_ = height;
  }
  if (stereo_pipeline_) return true;

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  stereo_sampler_ = device_->createSampler(sampler_desc);
  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 1, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 2, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 3, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    stereo_descriptors_[i] = device_->createDescriptorSet(set_desc);
    // One constant buffer per frame in flight: the mode is written each frame, and the previous frame may still be reading its own.
    stereo_constants_[i] = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(16, plume::RenderBufferFlag::CONSTANT));
    if (!stereo_descriptors_[i] || !stereo_constants_[i]) {
      KGFX_ERROR("stereo composite descriptor set creation failed; 3D is off");
      stereo_failed_ = true;
      return false;
    }
  }
  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = false;
  stereo_layout_ = device_->createPipelineLayout(layout_desc);
  const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
  std::unique_ptr<plume::RenderShader> vs = CreateBuiltinShader(device_.get(), format, shaders::g_stereoVSDxil, shaders::g_stereoVSSpirv, "VSMain");
  std::unique_ptr<plume::RenderShader> ps = CreateBuiltinShader(device_.get(), format, shaders::g_stereoPSDxil, shaders::g_stereoPSSpirv, "PSMain");
  if (!vs || !ps) {
    KGFX_ERROR("stereo composite shader creation failed; 3D is off");
    stereo_failed_ = true;
    return false;
  }
  stereo_pipeline_ = CreateFullscreenPipeline(device_.get(), stereo_layout_.get(), vs.get(), ps.get());
  if (!stereo_pipeline_) {
    KGFX_ERROR("stereo composite pipeline creation failed; 3D is off");
    stereo_failed_ = true;
    return false;
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    stereo_descriptors_[i]->setSampler(2, stereo_sampler_.get());
    stereo_descriptors_[i]->setBuffer(3, stereo_constants_[i].get(), 16);
  }
  KGFX_INFO("stereo composite ready ({})", format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  return true;
}

// Between the two eyes: the scene as the first eye saw it goes to the eye texture, and the scene target is cleared for the second.
void GraphicsSystem::StashStereoEye(plume::RenderCommandList* list, plume::RenderTexture* scene_texture, const plume::RenderFramebuffer* scene_framebuffer, uint32_t scene_width, uint32_t scene_height) {
  if (!stereo_eye_) return;
  if (active_scene_samples_ > 1) {
    const plume::RenderTextureBarrier to_resolve[] = {
        plume::RenderTextureBarrier(scene_texture, plume::RenderTextureLayout::RESOLVE_SOURCE),
        plume::RenderTextureBarrier(stereo_eye_.get(), plume::RenderTextureLayout::RESOLVE_DEST),
    };
    list->barriers(plume::RenderBarrierStage::COPY, to_resolve, uint32_t(std::size(to_resolve)));
    list->resolveTexture(stereo_eye_.get(), scene_texture);
    ++detail::g_frameProfile.msaa_resolves;
  } else {
    const plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(scene_texture, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(stereo_eye_.get(), plume::RenderTextureLayout::COPY_DEST),
    };
    list->barriers(plume::RenderBarrierStage::COPY, to_copy, uint32_t(std::size(to_copy)));
    list->copyTexture(stereo_eye_.get(), scene_texture);
  }
  const plume::RenderTextureBarrier to_draw[] = {
      plume::RenderTextureBarrier(scene_texture, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(active_scene_depth_, plume::RenderTextureLayout::DEPTH_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_draw, uint32_t(std::size(to_draw)));
  list->setFramebuffer(scene_framebuffer);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(scene_width), float(scene_height)));
  list->setScissors(plume::RenderRect(0, 0, scene_width, scene_height));
  // The same clears the frame began with: alpha to zero for the heat-haze mask, depth and stencil fresh.
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  scene_resolve_dirty_ = true;
  list->clearDepthStencil(true, true, 1.0f, 0);
}

// Resolves the second eye if the scene is multisampled, then draws both eyes onto the swap chain image in the display's stereo layout, letterboxed like the plain upscale.
void GraphicsSystem::CompositeStereo(plume::RenderCommandList* list, plume::RenderTexture* scene_texture, plume::RenderTexture* texture, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height) {
  plume::RenderTexture* right = scene_texture;
  plume::RenderTextureView* right_view = scene_color_view_.get();
  if (active_scene_samples_ > 1 && scene_resolved_) {
    const bool resolved = detail::ResolveSceneColorIfDirty(&scene_resolve_dirty_, [&] {
      const plume::RenderTextureBarrier to_resolve[] = {
          plume::RenderTextureBarrier(scene_texture, plume::RenderTextureLayout::RESOLVE_SOURCE),
          plume::RenderTextureBarrier(scene_resolved_.get(), plume::RenderTextureLayout::RESOLVE_DEST),
      };
      list->barriers(plume::RenderBarrierStage::COPY, to_resolve, uint32_t(std::size(to_resolve)));
      list->resolveTexture(scene_resolved_.get(), scene_texture);
    });
    ++(resolved ? detail::g_frameProfile.msaa_resolves : detail::g_frameProfile.msaa_resolve_reuses);
    right = scene_resolved_.get();
    right_view = scene_resolved_view_.get();
  }
  if (uint32_t* mapped = static_cast<uint32_t*>(stereo_constants_[frame_slot_]->map())) {
    mapped[0] = uint32_t(ActiveStereoMode());
    mapped[1] = mapped[2] = mapped[3] = 0;
    stereo_constants_[frame_slot_]->unmap();
  }
  const plume::RenderTextureBarrier to_composite[] = {
      plume::RenderTextureBarrier(stereo_eye_.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(right, plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(depth_texture_.get(), plume::RenderTextureLayout::DEPTH_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_composite, uint32_t(std::size(to_composite)));
  stereo_descriptors_[frame_slot_]->setTexture(0, stereo_eye_.get(), plume::RenderTextureLayout::SHADER_READ, stereo_eye_view_.get());
  stereo_descriptors_[frame_slot_]->setTexture(1, right, plume::RenderTextureLayout::SHADER_READ, right_view);
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
  list->setPipeline(stereo_pipeline_.get());
  list->setGraphicsPipelineLayout(stereo_layout_.get());
  list->setGraphicsDescriptorSet(stereo_descriptors_[frame_slot_].get(), 0);
  list->drawInstanced(3, 1, 0, 0);
}

}  // namespace kameo::gpu
