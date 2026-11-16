#pragma once

#include <string>

#include <imgui.h>

#include "ui/dialog.h"

// F7: every pixel shader the scene is drawing, with a switch for each, to find which one draws something (a shader
// switched off draws nothing). The hashes are what a shader mod targets, and the textures each samples are named by
// the hashes the texture dump uses.
class KameoShaderListOverlay : public KameoDialog {
 public:
  KameoShaderListOverlay();
  ~KameoShaderListOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  char filter_[64] = {};
  bool on_screen_only_ = true;
};
