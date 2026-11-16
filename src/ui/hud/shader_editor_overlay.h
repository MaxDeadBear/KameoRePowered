#pragma once

#include <memory>

#include <imgui.h>

#include "ui/dialog.h"

#ifdef _WIN32

// F1: the shader editor (src/shader_editor, also a program of its own) inside the game. The game is the preview, so
// there is no preview panel; what it saves or builds goes to shader_mods, which the game reloads as the files change.
// While its window shows the keyboard, mouse and pads belong to it, not the game. F1 or its close button hides the
// window and keeps the work.
class KameoShaderEditorOverlay : public KameoDialog {
 public:
  KameoShaderEditorOverlay();
  ~KameoShaderEditorOverlay() override;

  // Shows or hides the window. Hidden, the editor keeps its graph and goes on working: a build finishes and a waiting
  // live update is made, so the change can be watched in the game with the window out of the way.
  void SetVisible(bool visible) { visible_ = visible; }
  bool Visible() const { return visible_; }
  bool WantsExclusiveInput() const override { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
  bool visible_ = true;
};

#endif
