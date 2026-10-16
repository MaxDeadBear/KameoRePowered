#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace kameo::gpu {

// One decoded Bink frame, captured in the Draw_Bink_textures hook and drawn during the next present.
// The planes point straight into guest memory: Bink decodes on the CPU into D3DFMT_LIN_L8 surfaces, so there is no tiling or format conversion to undo.
struct BinkFrame {
  bool valid = false;
  bool has_alpha = false;
  uint32_t luma_width = 0;
  uint32_t luma_height = 0;
  uint32_t chroma_width = 0;
  uint32_t chroma_height = 0;
  const uint8_t* planes[4] = {};  // Y, Cr, Cb, A
  uint32_t pitches[4] = {};
  float vertices[4][5] = {};      // clip-space x,y,z + u,v, triangle strip
  float yuv_to_rgb[16] = {};      // the guest's own matrix
  // Set when the planes belong to a replacement movie frame instead of guest memory, keeping them alive until the upload.
  std::shared_ptr<const std::vector<uint8_t>> owned;
};

}  // namespace kameo::gpu
