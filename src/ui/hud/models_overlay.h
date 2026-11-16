#pragma once

#include <filesystem>
#include <memory>

#include <imgui.h>

#include "ui/dialog.h"

#ifdef _WIN32

// F11: the Models window. Lists the player's costumes (src/engine/dlc/mod_costumes.cpp) and lets a model file be put on
// one without leaving the game: wear it, reload it, import an .mdl or a glTF over it (a glTF is converted by the Python
// modkit, tools/modkit, run in the background), export it to glTF to edit elsewhere, or make a new costume from one of
// Kameo's skins. A file imported stays watched while "Reload when the file changes" is ticked for it.
// Windows only: the conversion starts the modkit with CreateProcess.
class KameoModelsOverlay : public KameoDialog {
 public:
  // game_root: the game's data folder, which holds the skin models a new costume is copied from.
  explicit KameoModelsOverlay(std::filesystem::path game_root);
  ~KameoModelsOverlay() override;

  void SetVisible(bool visible) { visible_ = visible; }
  bool Visible() const { return visible_; }
  bool WantsExclusiveInput() const override { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 public:
  struct State;  // the window's state, shared with its background jobs

 private:
  std::shared_ptr<State> state_;
  bool visible_ = true;
};

#endif
