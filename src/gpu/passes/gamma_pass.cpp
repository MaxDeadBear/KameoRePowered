#include "gpu/passes/gamma_pass.h"

#include <algorithm>
#include <iterator>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"
#include "gpu/shaders/builtin_shaders.h"

namespace kameo::gpu {

std::atomic<float> g_guestGammaExponent{1.0f};

float GraphicsSystem::GammaExponent() const {
  // Sample once for the frame: the guest may install a new ramp during replay.
  const float user_gamma = float(std::clamp(REXCVAR_GET(kameo_gfx_user_gamma), 20, 400)) / 100.0f;
  return (REXCVAR_GET(kameo_gfx_gamma) ? g_guestGammaExponent.load() : 1.0f) / user_gamma;
}

// Compose directly into a native-size UNORM target, then apply the scanout curve
// to the swap chain. This keeps scene, video and UI ordering without a frame copy.
bool GraphicsSystem::EnsureGammaResources(uint32_t width, uint32_t height, float exponent) {
  if (gamma_failed_) {
    return false;
  }

  const bool source_changed = !gamma_source_ || gamma_width_ != width || gamma_height_ != height;
  if (source_changed) {
    // The source and its descriptor may still be read by an earlier frame.
    WaitForAllFrames();
    gamma_framebuffer_.reset();
    gamma_source_view_.reset();
    gamma_source_.reset();
    gamma_source_ = device_->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kColorFormat));
    if (!gamma_source_) {
      KGFX_ERROR("gamma scratch texture {}x{} failed", width, height);
      gamma_failed_ = true;
      return false;
    }
    gamma_source_view_ = gamma_source_->createTextureView(
        plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::B8G8R8A8_UNORM));
    if (!gamma_source_view_) {
      KGFX_ERROR("gamma source view creation failed");
      gamma_failed_ = true;
      return false;
    }
    gamma_width_ = width;
    gamma_height_ = height;
  }

  const bool pipeline_changed = !gamma_pipeline_;
  if (pipeline_changed) {
    gamma_sampler_ = device_->createSampler(plume::RenderSamplerDesc());
    plume::RenderDescriptorRange ranges[] = {
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1),
    };
    plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
    gamma_descriptors_ = device_->createDescriptorSet(set_desc);

    plume::RenderPipelineLayoutDesc layout_desc;
    layout_desc.descriptorSetDescs = &set_desc;
    layout_desc.descriptorSetDescsCount = 1;
    // The vertex shader builds its triangle from SV_VertexID, so there is no vertex buffer and no input layout.
    layout_desc.allowInputLayout = false;
    gamma_layout_ = device_->createPipelineLayout(layout_desc);
    if (!gamma_sampler_ || !gamma_descriptors_ || !gamma_layout_) {
      KGFX_ERROR("gamma layout or descriptors creation failed");
      gamma_failed_ = true;
      return false;
    }

    const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
    std::unique_ptr<plume::RenderShader> vs = CreateBuiltinShader(device_.get(), format, shaders::g_gammaVSDxil, shaders::g_gammaVSSpirv, "VSMain");
    std::unique_ptr<plume::RenderShader> ps = CreateBuiltinShader(device_.get(), format, shaders::g_gammaPSDxil, shaders::g_gammaPSSpirv, "PSMain");
    if (!vs || !ps) {
      KGFX_ERROR("gamma shader creation failed");
      gamma_failed_ = true;
      return false;
    }

    gamma_pipeline_ = CreateFullscreenPipeline(device_.get(), gamma_layout_.get(), vs.get(), ps.get());
    if (!gamma_pipeline_) {
      KGFX_ERROR("gamma pipeline creation failed");
      gamma_failed_ = true;
      return false;
    }

    gamma_constants_ = device_->createBuffer(
        plume::RenderBufferDesc::UploadBuffer(16, plume::RenderBufferFlag::CONSTANT));
    if (!gamma_constants_) {
      KGFX_ERROR("gamma constants creation failed");
      gamma_failed_ = true;
      return false;
    }
    gamma_descriptors_->setSampler(1, gamma_sampler_.get());
    gamma_descriptors_->setBuffer(2, gamma_constants_.get(), 16);
    KGFX_INFO("gamma pass ready ({})",
                format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  }

  // Avoid rewriting an unchanged descriptor while another frame uses it.
  if (source_changed || pipeline_changed) {
    gamma_descriptors_->setTexture(0, gamma_source_.get(), plume::RenderTextureLayout::SHADER_READ,
                                   gamma_source_view_.get());
  }
  if (!gamma_framebuffer_) {
    const plume::RenderTexture* color = gamma_source_.get();
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = depth_texture_.get();
    desc.depthAttachmentView = depth_view_.get();
    gamma_framebuffer_ = device_->createFramebuffer(desc);
    if (!gamma_framebuffer_) {
      KGFX_ERROR("gamma composition framebuffer creation failed");
      gamma_failed_ = true;
      return false;
    }
  }

  // Written only when the exponent changes, behind a full GPU wait, because a frame in flight may be reading the buffer through a descriptor set that cannot rotate.
  if (!gamma_constants_written_ || exponent != gamma_constants_shadow_) {
    WaitForAllFrames();
    if (float* mapped = static_cast<float*>(gamma_constants_->map())) {
      mapped[0] = exponent;
      mapped[1] = 1.0f;  // g_Brightness, a multiplier the shader applies after the curve
      mapped[2] = mapped[3] = 0.0f;
      gamma_constants_->unmap();
      gamma_constants_shadow_ = exponent;
      gamma_constants_written_ = true;
    } else {
      return false;  // Keep rendering directly to the window if preparation fails.
    }
  }
  return true;
}

void GraphicsSystem::ApplyGamma(plume::RenderCommandList* list, plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height) {
  // EnsureGammaResources prepared the frame's sampled exponent before routing
  // any pixels into gamma_source_; it already contains the complete composition.
  const plume::RenderTextureBarrier to_draw[] = {
      plume::RenderTextureBarrier(gamma_source_.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_draw, uint32_t(std::size(to_draw)));

  list->setFramebuffer(framebuffer);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  list->setScissors(plume::RenderRect(0, 0, width, height));
  list->setPipeline(gamma_pipeline_.get());
  list->setGraphicsPipelineLayout(gamma_layout_.get());
  list->setGraphicsDescriptorSet(gamma_descriptors_.get(), 0);
  list->drawInstanced(3, 1, 0, 0);
}

}  // namespace kameo::gpu
