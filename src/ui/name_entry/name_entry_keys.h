#pragma once

#include <imgui.h>

#include "ui/dialog.h"

// Feeds a physical keyboard into the in-game name entry while it is open, and the key being bound into a binding capture; it draws nothing itself.
class KameoNameEntryKeys : public KameoDialog {
 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static void FeedKeyCapture();
};
