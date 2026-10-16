#pragma once

// The ImGui bridge for the plume renderer: ReXApp creates no drawer for a graphics system that exists but has no presenter, so the app supplies this one, calls SetupOverlays(nullptr, drawer) and drives ImGuiDrawer::Draw itself.

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>

#include "gpu/graphics_system.h"

namespace kameo::gpu {

// UIDrawContext's presenter-less constructor is protected and exists for app-driven overlays, so it only needs exposing.
class UiDrawContext final : public rex::ui::UIDrawContext {
 public:
  UiDrawContext(uint32_t width, uint32_t height) : rex::ui::UIDrawContext(width, height) {}
};

class PlumeImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit PlumeImmediateDrawer(GraphicsSystem* graphics) : graphics_(graphics) {}

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool is_repeated, const uint8_t* data) override;
  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width, float coordinate_space_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
  void Draw(const rex::ui::ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  GraphicsSystem* graphics_ = nullptr;

  // The coordinate space the current Begin established; positions are folded into clip space on the CPU because the UI vertex shader passes them through.
  float space_width_ = 1.0f;
  float space_height_ = 1.0f;
  uint32_t target_width_ = 0;
  uint32_t target_height_ = 0;

  // The batch currently open between BeginDrawBatch and EndDrawBatch.
  std::vector<rex::ui::ImmediateVertex> batch_vertices_;
  std::vector<uint16_t> batch_indices_;

  GraphicsSystem::UiFrame frame_;
};

}  // namespace kameo::gpu
