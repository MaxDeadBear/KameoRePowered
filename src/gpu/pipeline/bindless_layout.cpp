#include "gpu/graphics_system.h"

#include <iterator>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/graphics_system_internal.h"
#include "gpu/log.h"

namespace kameo::gpu {

// The translated shaders dictate this layout: unbounded Texture2D, Texture3D and TextureCube arrays in spaces 0-2, samplers in space 3, and three constant blocks in space 4.
bool GraphicsSystem::EnsureDrawPathResources() {
  if (draw_path_ready_) {
    return true;
  }
  if (draw_path_failed_ || !device_) {
    return false;
  }

  // Bindless needs descriptor indexing; fail loudly rather than build a layout that silently misbehaves.
  if (!device_->getCapabilities().descriptorIndexing) {
    KGFX_ERROR("device does not support descriptor indexing; bindless path unavailable");
    draw_path_failed_ = true;
    return false;
  }

  // Each bindless set takes kMaxTextures out of plume's single 65536-entry shader-visible heap, so 3 * 16384 leaves room for everything else; 32768 overflowed it and crashed on launch.
  constexpr uint32_t kMaxTextures = 16384;
  constexpr uint32_t kMaxSamplers = 256;

  const plume::RenderDescriptorRange tex2d(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange tex3d(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange cube(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange samp(plume::RenderDescriptorRangeType::SAMPLER, 0, kMaxSamplers);

  // Set index == HLSL space, so the order here is load-bearing.
  const plume::RenderDescriptorSetDesc set_descs[] = {
      plume::RenderDescriptorSetDesc(&tex2d, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&tex3d, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&cube, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&samp, 1, true, kMaxSamplers),
  };

  // The space4 constant buffers are root descriptors, so the draw path points them at each draw's slice of its own upload ring rather than binding a descriptor set per draw.
  const plume::RenderRootDescriptorDesc root_descs[] = {
      plume::RenderRootDescriptorDesc(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
      plume::RenderRootDescriptorDesc(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
      plume::RenderRootDescriptorDesc(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
  };

  // XenosRecomp's SPIR-V reads those blocks through vk::RawBufferLoad from three buffer addresses in a 24-byte push constant range, and plume's Vulkan setGraphicsRootDescriptor is assert(false).
  const bool spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  const plume::RenderPushConstantRange push_ranges[] = {
      plume::RenderPushConstantRange(0, 4, 0, uint32_t(sizeof(uint64_t) * 3),
                                     plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL | plume::RenderShaderStageFlag::GEOMETRY),
  };

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = set_descs;
  layout_desc.descriptorSetDescsCount = uint32_t(std::size(set_descs));
  if (spirv) {
    layout_desc.pushConstantRanges = push_ranges;
    layout_desc.pushConstantRangesCount = uint32_t(std::size(push_ranges));
  } else {
    layout_desc.rootDescriptorDescs = root_descs;
    layout_desc.rootDescriptorDescsCount = uint32_t(std::size(root_descs));
  }
  layout_desc.allowInputLayout = true;
  draw_layout_ = device_->createPipelineLayout(layout_desc);
  if (!draw_layout_) {
    KGFX_ERROR("bindless pipeline layout creation failed");
    draw_path_failed_ = true;
    return false;
  }

  draw_texture_set_ = device_->createDescriptorSet(set_descs[0]);
  draw_texture3d_set_ = device_->createDescriptorSet(set_descs[1]);
  draw_cube_set_ = device_->createDescriptorSet(set_descs[2]);
  draw_sampler_set_ = device_->createDescriptorSet(set_descs[3]);

  if (!draw_texture_set_ || !draw_texture3d_set_ || !draw_cube_set_ || !draw_sampler_set_) {
    KGFX_ERROR("bindless descriptor sets failed");
    draw_path_failed_ = true;
    return false;
  }

  draw_path_ready_ = true;
  KGFX_INFO("bindless draw path ready ({} texture slots, {} samplers)", kMaxTextures,
              kMaxSamplers);
  return true;
}

bool GraphicsSystem::GetDrawPathContext(DrawPathContext* out) {
  if (!out || !draw_path_ready_ || !device_) {
    return false;
  }
  out->device = device_.get();
  out->layout = draw_layout_.get();
  out->texture_set = draw_texture_set_.get();
  out->texture3d_set = draw_texture3d_set_.get();
  out->cube_set = draw_cube_set_.get();
  out->sampler_set = draw_sampler_set_.get();
  out->scene_depth = active_scene_depth_ ? active_scene_depth_ : depth_texture_.get();
  out->scene_samples = active_scene_samples_;
  out->scene_resolved = active_scene_samples_ > 1 ? scene_resolved_.get() : nullptr;
  out->scene_target_generation = scene_target_generation_;
  out->color_format = uint32_t(kColorFormat);
  out->depth_format = uint32_t(kDepthFormat);
  out->frame_slot = frame_slot_;
  out->frame_index = frame_index_;
  out->spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  return true;
}

}  // namespace kameo::gpu
