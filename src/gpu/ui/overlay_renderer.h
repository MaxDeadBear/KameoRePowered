#pragma once

// Draws the SDK's overlay frame (ImGui: our dialogs, the console, the debug overlay) with the native renderer. The app
// hands it to IUISystem::DrawOverlays once per presented frame.

#include <rex/overlay_renderer.h>

#include "gpu/graphics_system.h"

namespace kameo::gpu {

class OverlayRenderer final : public rex::IOverlayRenderer {
 public:
  explicit OverlayRenderer(GraphicsSystem* graphics) : graphics_(graphics) {}

  // The size of the target the frame is drawn into, in pixels; set before each DrawOverlays.
  void SetTargetSize(uint32_t width, uint32_t height) {
    target_width_ = width;
    target_height_ = height;
  }

  void RenderOverlays(ImDrawData& draw_data) override;

 private:
  void UpdateTextures(ImDrawData& draw_data);

  GraphicsSystem* graphics_ = nullptr;
  uint32_t target_width_ = 0;
  uint32_t target_height_ = 0;
  std::vector<uint8_t> expanded_;  // an Alpha8 texture widened to RGBA
};

}  // namespace kameo::gpu
