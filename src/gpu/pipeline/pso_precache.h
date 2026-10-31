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
// Called once a frame. Compiles the recipes on low-priority background threads, so frames and the game never wait on a cold driver cache; a draw that needs a pipeline first builds it itself. If no thread can start, the render thread works through them a slice a frame.
void PrecacheDrawPipelines(GraphicsSystem& gfx);
// How far the pipelines have got; false once they are finished or were never due. Callable from any thread.
bool PipelinePrecacheProgress(uint32_t* done, uint32_t* total);

// The first scene draw or video frame of the run: the end of the black screen.
void NoteGamePicture();
bool GamePictureShown();

}  // namespace kameo::gpu
