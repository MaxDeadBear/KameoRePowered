#pragma once

#include <atomic>

#include <array>
#include <cstdint>
#include <vector>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

bool LetterboxScene();
// Whether a loading screen (the level fade model) was drawn in the last moments; widescreen leaves those at 16:9.
bool LoadingScreenShowing();

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
// `artwork`: a full-screen picture (ClusterHudBoxes marks it 2), kept at the 16:9 shape it was drawn for, centred.
bool PlaceHudBox(const float* box, float stretch, HudPlacement* out, bool artwork = false);
// The storybook's menu cards (the pause card's parchment and the bar under it), drawn in 3D through the menu's own 16:9
// camera: placed like the text on them, at their 16:9 shape about the screen's centre.
// A pixel shader of 0 places any draw so (the loading screen's book and text, DrawCall::loading_layer).
bool PlaceMenuCard(uint64_t pixel_shader, float stretch, HudPlacement* out);
// The loading screen's smoke backdrop on a wider screen: the whole width, scaled as much in height about the centre.
bool PlaceLoadingBackdrop(float stretch, HudPlacement* out);
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
// With kameo_dev_log_hud on, asks the next frame to log every draw it replays (F9).
void RequestHudFrameDump();
// From the game thread each time the level fade model (the loading screen's logo, backdrop and hint) is drawn.
void NoteLoadingScreenDrawn();
extern std::atomic<bool> g_hudFrameDumpRequested;

}  // namespace kameo::gpu
