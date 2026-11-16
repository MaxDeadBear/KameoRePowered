#pragma once

#include <imgui.h>

#include "ui/dialog.h"

// F1 toggles the in-game FPS counter, the same switch as Video Options' FPS Counter; it draws nothing itself.
class KameoFpsOverlay : public KameoDialog {
 protected:
  void OnDraw(ImGuiIO& io) override;
};
