#pragma once

#include "gpu/draw/draw_types.h"

namespace kameo::gpu {

inline uint64_t PipelineLayoutKey(const HostLayout& layout) {
  uint64_t key = 1469598103934665603ull;
  for (const HostElement& element : layout.elements) {
    for (uint32_t value : {element.usage, element.usage_index, uint32_t(element.format), element.dst_offset}) {
      key = (key ^ value) * 1099511628211ull;
    }
  }
  return key;
}

struct PipelineRecipe {
  uint64_t vs_hash;
  uint64_t ps_hash;
  uint32_t layout_index;
  uint32_t primitive;
  uint32_t depth_control;
  uint32_t blend_control;
  uint32_t color_control;
  uint32_t mode_control;
  uint32_t color_mask;
  uint32_t alpha_to_mask;
  bool depth_only;
  float poly_offset;
  float poly_offset_scale;
};

class GraphicsSystem;
void QueuePipelinePrecacheShaders(GraphicsSystem& gfx);
void PrecacheDrawPipelines(GraphicsSystem& gfx);

}  // namespace kameo::gpu
