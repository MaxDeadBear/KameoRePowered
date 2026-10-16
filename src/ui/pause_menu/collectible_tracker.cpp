// Collectibles: what this area still holds, the game's totals, and how many a sequence break left behind for the tribe shops.

#include <array>
#include <string>

#include "core/unlock_codes.h"
#include "engine/collectibles.h"
#include "ui/pause_menu/pause_menu_internal.h"

namespace kameo::ui {
namespace {

using engine::CollectibleKind;

struct KindNames {
  const char16_t* here;
  const char16_t* all;
};
constexpr std::array<KindNames, 4> kKindNames = {{
    {u"Fruit in This Area", u"All Elemental Fruit"},
    {u"Elixirs in This Area", u"All Elixirs of Life"},
    {u"Crystal Eye in This Area", u"All Crystal Eyes"},
    {u"Costumes in This Area", u"All Costumes"},
}};

struct Tally {
  uint32_t found = 0;
  uint32_t total = 0;
};

std::u16string OutOf(const Tally& tally) { return Number(int32_t(tally.found), u" / ") + Number(int32_t(tally.total), u""); }

}  // namespace

bool AddCollectibleRows(PPCContext& ctx, uint8_t* base, uint32_t window, Page page) {
  if (page != Page::Collectibles) return false;
  std::array<Tally, 4> here{};
  std::array<Tally, 4> all{};
  uint32_t missed = 0;
  const uint32_t level = engine::CurrentLevelToken();
  for (size_t i = 0; i < engine::kCollectibleCount; ++i) {
    const engine::Collectible& item = engine::kCollectibles[i];
    const size_t kind = size_t(item.kind);
    // Fruit count by value, as the game's own total of 100 does.
    const uint32_t weight = item.kind == CollectibleKind::Fruit ? item.value : 1;
    const bool found = engine::CollectibleFound(ctx, base, item);
    all[kind].total += weight;
    if (found) all[kind].found += weight;
    if (item.level == level) {
      here[kind].total += weight;
      if (found) here[kind].found += weight;
    }
    if (engine::CollectibleMissed(ctx, base, item)) ++missed;
  }
  AddPageTitle(ctx, base, window, u"Collectibles");
  uint32_t row = 0;
  bool anything_here = false;
  for (size_t kind = 0; kind < kKindNames.size(); ++kind) {
    if (here[kind].total == 0) continue;
    anything_here = true;
    AddToggleRow(ctx, base, window, kItemCollectibleRow + row++, kKindNames[kind].here, GuestWideString(OutOf(here[kind])));
  }
  if (!anything_here) AddToggleRow(ctx, base, window, kItemCollectibleRow + row++, u"This Area", GuestWideString(u"Nothing to find"));
  for (size_t kind = 0; kind < kKindNames.size(); ++kind) AddToggleRow(ctx, base, window, kItemCollectibleRow + row++, kKindNames[kind].all, GuestWideString(OutOf(all[kind])));
  AddToggleRow(ctx, base, window, kItemCollectibleRow + row++, u"Missed, Sold in Tribe Shops", GuestWideString(Number(int32_t(missed), u"")));
  return true;
}

bool PressCollectibleItem(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t id) {
  if (id == kItemCollectiblesPage) {
    ActivatePage(ctx, base, pause, window, Page::Collectibles);
    return true;
  }
  return id >= kItemCollectibleRow && id < kItemCollectibleRow + 16;
}

bool BackFromCollectibles(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, Page page) {
  if (page != Page::Collectibles) return false;
  ActivatePage(ctx, base, pause, window, Page::Gameplay);
  // Back onto the Collectibles link, which follows the page's options and Level Select's link when that is unlocked.
  KeepCursor(ctx, base, window, VisibleOptionCount(Page::Gameplay) + (KameoCodeUnlocked(KameoUnlockCode::LevelSelect) ? 1 : 0));
  return true;
}

}  // namespace kameo::ui
