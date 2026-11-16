// The achievement art beside the Achievements page's rows: the game's text window cannot hold images, so the icons are drawn over it where the window drew each row.
#pragma once

#include <imgui.h>

#include "ui/dialog.h"
#include "ui/hud/achievement_icon_set.h"

class KameoAchievementIcons : public KameoDialog {
 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  KameoAchievementIconSet icons_;
};
