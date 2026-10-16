#pragma once

#include <imgui.h>
#include <rex/rex_app.h>

// F1 toggles the in-game FPS counter, the same switch as Video Options' FPS Counter; it draws nothing itself.
class KameoFpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit KameoFpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};
