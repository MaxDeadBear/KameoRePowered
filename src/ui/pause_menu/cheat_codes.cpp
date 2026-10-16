// Cheat codes: eight presses of X, Y and the bumpers on any pause or options page, each revealing a hidden option for good.

#include <array>

#include <rex/logging.h>

#include "core/unlock_codes.h"
#include "input/guest_controls.h"
#include "ui/pause_menu/pause_menu_internal.h"

namespace kameo::ui {
namespace {

using kameo::input::kButtonX;
using kameo::input::kButtonY;
constexpr uint32_t kButtonLB = 0x0100;
constexpr uint32_t kButtonRB = 0x0200;
// The tribe shops' purchase sound.
constexpr uint32_t kCodeAcceptedSound = 0x679D554;

struct CheatCode {
  KameoUnlockCode feature;
  const char* name;
  std::array<uint32_t, 8> buttons;
};
constexpr CheatCode kCodes[] = {
    {KameoUnlockCode::LevelSelect, "Level Select", {kButtonLB, kButtonRB, kButtonLB, kButtonRB, kButtonX, kButtonY, kButtonX, kButtonY}},
    {KameoUnlockCode::GameCheats, "Unlock All Extras", {kButtonX, kButtonX, kButtonY, kButtonY, kButtonLB, kButtonLB, kButtonRB, kButtonRB}},
    {KameoUnlockCode::Warriors, "All Warriors & Upgrades", {kButtonY, kButtonX, kButtonY, kButtonX, kButtonRB, kButtonLB, kButtonRB, kButtonLB}},
    {KameoUnlockCode::CollectibleTracker, "Collectibles", {kButtonX, kButtonY, kButtonLB, kButtonRB, kButtonX, kButtonY, kButtonLB, kButtonRB}},
};

std::array<uint32_t, 8> g_entered{};

}  // namespace

void TickCheatCodes(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t pressed) {
  bool unlocked = false;
  for (const uint32_t button : {kButtonX, kButtonY, kButtonLB, kButtonRB}) {
    if ((pressed & button) == 0) continue;
    for (size_t i = 0; i + 1 < g_entered.size(); ++i) g_entered[i] = g_entered[i + 1];
    g_entered.back() = button;
    for (const CheatCode& code : kCodes) {
      if (g_entered != code.buttons || KameoCodeUnlocked(code.feature)) continue;
      KameoUnlockFeature(code.feature);
      REXLOG_INFO("[kameo] cheat code entered: {}", code.name);
      g_entered = {};
      unlocked = true;
    }
  }
  if (!unlocked) return;
  CallGuest(ctx, base, sub_822EA728, kCodeAcceptedSound);
  // The newly revealed row or link shows at once on the page it belongs to.
  if (window && g_currentPage != Page::None) {
    const uint32_t index = SelectedIndex(base, window);
    ActivatePage(ctx, base, pause, window, g_currentPage);
    KeepCursor(ctx, base, window, index);
  }
}

}  // namespace kameo::ui
