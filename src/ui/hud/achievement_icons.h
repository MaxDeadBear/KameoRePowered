// The achievement art beside the Achievements page's rows: the game's text window cannot hold images, so the icons are drawn over it where the window drew each row.
#pragma once

#include <memory>

#include <imgui.h>
#include <rex/rex_app.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

class KameoAchievementIcons : public rex::ui::ImGuiDialog {
 public:
  KameoAchievementIcons(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer) : rex::ui::ImGuiDialog(drawer), immediate_drawer_(immediate_drawer) {}
  // The icons come out of the title's XDBF through the runtime, which exists only after setup.
  void SetRuntime(rex::Runtime* runtime) { runtime_ = runtime; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::ui::ImmediateDrawer* immediate_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<rex::ui::AchievementIconCache> cache_;
};
