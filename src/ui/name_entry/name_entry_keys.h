#pragma once

#include <imgui.h>
#include <rex/rex_app.h>

// Feeds a physical keyboard into the in-game name entry while it is open, and the key being bound into a binding capture; it draws nothing itself.
class KameoNameEntryKeys : public rex::ui::ImGuiDialog {
 public:
  explicit KameoNameEntryKeys(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static void FeedKeyCapture();
};
