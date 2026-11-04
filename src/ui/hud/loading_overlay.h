// A small panel over the black screen before the game's first picture, in the launcher's indigo and gold: the shaders being prepared, then the game loading.
#pragma once

#include <chrono>

#include <rex/ui/imgui_dialog.h>

// The pause book's faces ConfigureFonts adds from the fonts folder: IM FELL English small capitals, roman and italic (null when
// the folder is missing).
ImFont* KameoBookCapsFont();
ImFont* KameoBookTextFont();
ImFont* KameoBookItalicFont();

class KameoLoadingOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit KameoLoadingOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer), start_(std::chrono::steady_clock::now()) {}

  // Adds the wordmark's face to the overlays' font atlas, as the drawer is made.
  static void ConfigureFonts(ImFontAtlas* atlas);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::chrono::steady_clock::time_point start_;
  // When the panel started to fade out, in seconds from start_; negative until then.
  float hide_at_ = -1.0f;
  bool finished_ = false;
};
