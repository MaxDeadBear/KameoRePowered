// The parchment card (kameo_menu_style 1): the pause menu redrawn on the game's own parchment card, from the game's own art
// (the card, the logo, the warriors' and characters' faces, the buttons; kept from the player's copy as it loads, never
// shipped) in the IM FELL faces. A short main page and Options on cards of their own made for this menu, Options as one card with its
// categories as tabs, the quit choices and the achievements. Pages the card has no layout of its own for (Level Select,
// Collectibles) are the storybook's, opened from the card, which comes back when they return to the main page.
//
// The game thread owns its state, as it does the storybook's: it opens and closes it from the pause object's tick, takes
// the controllers from XInputGetState while it is open, changes options and plays the game's sounds. The card only draws a
// snapshot of that state and posts mouse clicks back.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <imgui.h>
#include <rex/rex_app.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

struct PPCContext;

// Before the game starts: asks the texture loader to keep the card's art.
void KameoKeepPauseCardArt();
// Whether the pause menu is the card rather than the storybook's own.
bool KameoPauseCardStyle();
bool KameoPauseCardOpen();
// Game thread, from the pause object's tick: opens the card over the main pause page, and back over it when a page the card
// opened returns there. `page_id` is the pause record's page (+120).
void KameoPauseCardTick(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t page_id);
// Game thread, from XInputGetState with the state (a guest address) the game is about to get for `user`: while the card is
// open it takes that input and leaves the game a pad at rest. Returns whether it took it.
bool KameoPauseCardTakePad(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state);

class KameoPauseCard : public rex::ui::ImGuiDialog {
 public:
  KameoPauseCard(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer) : rex::ui::ImGuiDialog(drawer), immediate_drawer_(immediate_drawer) {}
  // The achievements' art comes out of the title's XDBF through the runtime, which exists only after setup.
  void SetRuntime(rex::Runtime* runtime) { runtime_ = runtime; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // The art as textures, made once each has loaded.
  rex::ui::ImmediateTexture* Art(uint64_t hash);
  rex::ui::ImmediateTexture* AchievementIcon(uint32_t id);
  // The cards made for this menu (res/ui): 0 the main page's, 1 Options'. Each is loaded once.
  rex::ui::ImmediateTexture* OwnCard(int which);
  rex::ui::ImmediateDrawer* immediate_drawer_;
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<rex::ui::AchievementIconCache> icons_;
  std::unordered_map<uint64_t, std::unique_ptr<rex::ui::ImmediateTexture>> art_;
  std::unique_ptr<rex::ui::ImmediateTexture> own_cards_[2];
  bool own_card_tried_[2] = {};
  // Eased positions, so the highlight glides between lines.
  float main_mark_ = -1.0f;
  float option_mark_ = -1.0f;
  int showed_sheet_ = -1;
  ImVec2 last_mouse_ = ImVec2(-1.0f, -1.0f);
};
