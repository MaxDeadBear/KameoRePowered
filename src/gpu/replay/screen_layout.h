#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

bool LetterboxScene();

// The uniform scale and letterbox offset that place the guest frame in a host image; identity for offscreen targets.
struct GuestToHost {
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
};
GuestToHost GuestToHostTransform(uint32_t host_w, uint32_t host_h);

// A clip-space affine x' = mx * x + bx, y' = my * y + by for one HUD draw.
struct HudPlacement {
  float mx = 1.0f;
  float bx = 0.0f;
  float my = 1.0f;
  float by = 0.0f;
};

// A 2D draw's clip-space box (x0, x1, y0, y1) through its c0-c3 matrix, for draws that neither test nor write depth.
bool HudDrawBox(const DrawCall& call, const uint8_t* vertices, const uint32_t* indices, const float* vs_constants, float* box);
// Places a 2D box by where it lands per kameo_hud_scale and kameo_hud_layout; full-screen boxes keep the frame's own mapping.
bool PlaceHudBox(const float* box, float stretch, HudPlacement* out);
// Where a rect in guest frame coordinates lands, normalised, in a host image the draw path scaled and letterboxed.
void GuestRectToHostUv(uint32_t host_w, uint32_t host_h, float gx, float gy, float gw, float gh, float* out);
// Each HUD draw's clip-space box, unioned across the draws of one text window and draws overlapping on one target, so an element and its markers move as one piece.
void ClusterHudBoxes(const DrawFrame& frame, std::vector<std::array<float, 4>>& hud_boxes, std::vector<uint8_t>& hud_boxed);

}  // namespace kameo::gpu::detail

namespace kameo::gpu {

// Maps a point in the window's physical pixels to the 640x480 layout the game's text windows use, following the letterboxing the draw path applies.
bool WindowPointToUiLayout(float window_x, float window_y, uint32_t window_width, uint32_t window_height, float* layout_x, float* layout_y);
// While a menu with selectable lines is open the HUD layout only unstretches, so the lines stay where the pointer's hit test expects them.
void SetHudMenuOpen(bool open);

}  // namespace kameo::gpu
