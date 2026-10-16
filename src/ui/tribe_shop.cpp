// The tribe shops also sell back, for score, any collectible a sequence break left behind: one row per kind after the shop's own items.

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "engine/collectibles.h"
#include "engine/guest_addresses.h"
#include "ui/text_window.h"

namespace kameo::ui {
namespace {

using engine::Collectible;
using engine::CollectibleKind;

constexpr uint32_t kShopSelectedSlot = 0x82B715D0;
constexpr uint32_t kCurrentTribe = 0x82B71584;
constexpr uint32_t kFairyCastleGrounds = 0x03D41FF3;
constexpr uint32_t kLevelClass = 0x72CCBC;
constexpr uint32_t kMsgGetMenuWindow = 0x190697;
constexpr uint32_t kMsgShowPreview = 0x484473D;
constexpr uint32_t kMsgSendPreviewOff = 0x40CAF0D;
constexpr uint32_t kPurchaseSound = 0x679D554;
constexpr uint32_t kTribeTextStyleObjType = 0x1A0;
constexpr uint32_t kTribeMenuSelected = 0x821246D8;
constexpr int32_t kBuyablePrefixString = 4188;
constexpr int32_t kRowPrefixString = 79;
constexpr int32_t kYesString = 867;
constexpr int32_t kNoString = 919;
constexpr uint32_t kListPage = 1;
constexpr uint32_t kConfirmPage = 2;
constexpr uint32_t kBuyPage = 3;
// Past the parameter save area, which objectAskObject and objectAskClass spill their variadic registers into.
constexpr uint32_t kScratchBuffer = 0x100;
// Any page the shop does not know runs only its preamble, which clears both windows.
constexpr uint32_t kClearOnlyPage = 99;

struct KindRow {
  CollectibleKind kind;
  const char16_t* name;
  const char* preview;
  double scale;
  double y_offset;
};
// The previews are the ones the shops already show for fruit and elixirs.
constexpr std::array<KindRow, 4> kKindRows = {{
    {CollectibleKind::Fruit, u"Elemental Fruit", "MonsterFood", 0.9, -0.3},
    {CollectibleKind::Elixir, u"Elixir of Life", "EnergyBottle", 1.0, 0.0},
    {CollectibleKind::CrystalEye, u"Crystal Eye", nullptr, 0.0, 0.0},
    {CollectibleKind::Costume, u"Costume", nullptr, 0.0, 0.0},
}};

struct ExtraRow {
  const KindRow* kind;
  const Collectible* next;
  uint32_t missed;
  uint32_t price;
};

std::vector<ExtraRow> ExtraRows(PPCContext& ctx, uint8_t* base) {
  std::vector<ExtraRow> rows;
  for (const KindRow& kind : kKindRows) {
    ExtraRow row{&kind, nullptr, 0, 0};
    for (size_t i = 0; i < engine::kCollectibleCount; ++i) {
      const Collectible& item = engine::kCollectibles[i];
      if (item.kind != kind.kind || !engine::CollectibleMissed(ctx, base, item)) continue;
      if (!row.next) row.next = &item;
      ++row.missed;
    }
    if (row.next) {
      row.price = engine::CollectiblePrice(base, *row.next);
      rows.push_back(row);
    }
  }
  return rows;
}

// The shop's own rows: six, but FairyCastleGrounds leaves out its empty sixth.
uint32_t OwnRowCount() { return engine::CurrentLevelToken() == kFairyCastleGrounds ? 5 : 6; }

uint32_t MenuWindow(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = engine::ScratchFrame(ctx.r1.u32) + kScratchBuffer;
  REX_STORE_U32(out, 0);
  CallGuest(ctx, base, sub_82251E88, kLevelClass, 0, kMsgGetMenuWindow, out);
  return REX_LOAD_U32(out);
}

void ShowPreview(PPCContext& ctx, uint8_t* base, const KindRow* kind) {
  PPCContext call = ctx;
  call.r1.u64 = engine::ScratchFrame(ctx.r1.u32);
  const uint32_t name = uint32_t(call.r1.u64) + kScratchBuffer;
  call.r3.u64 = REX_LOAD_U32(kCurrentTribe);
  call.r4.u64 = 0;
  call.r5.u64 = kMsgShowPreview;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.u64 = 0;
  if (kind && kind->preview) {
    std::memcpy(base + name, kind->preview, std::strlen(kind->preview) + 1);
    call.r6.u64 = name;
    std::memcpy(&call.r7.u64, &kind->scale, sizeof(double));
    std::memcpy(&call.r8.u64, &kind->y_offset, sizeof(double));
  }
  sub_82251DF8(call, base);
}

void SelectRow(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t row) {
  REX_STORE_U8(MenuOf(base, TextObjectOf(base, window)) + kMenuSelectedIndex, uint8_t(row));
  PPCContext call = ctx;
  call.r3.u64 = window;
  call.r4.u64 = row;
  call.f1.f64 = 0.0;
  sub_8210ACA0(call, base);
}

void AddExtraRows(PPCContext& ctx, uint8_t* base, const std::vector<ExtraRow>& rows) {
  const uint32_t window = MenuWindow(ctx, base);
  if (!window) return;
  const uint32_t prefix = LocalisedString(ctx, base, kBuyablePrefixString);
  for (const ExtraRow& row : rows) {
    const uint32_t text_object = TextObjectOf(base, window);
    REX_STORE_U32(text_object + kTextColourCallback, kTextDefaultMenuColourCallback);
    REX_STORE_U32(text_object + kTextColourCallbackData, 0);
    const std::u16string label = std::u16string(u"Missed ") + row.kind->name + u" (" + Number(int32_t(row.missed), u")") + u": " + Number(int32_t(row.price), u" points");
    if (const uint32_t text = GuestWideString(label)) AddTextItem(ctx, base, window, 0, prefix, text, kTribeMenuSelected, true);
  }
}

// The game's add-text call for the tribe's speech box, with the box geometry the shop's confirm page passes.
void AddSpeech(PPCContext& ctx, uint8_t* base, uint32_t tribe, uint32_t text) {
  PPCContext style = ctx;
  style.r3.u64 = REX_LOAD_U32(tribe + kTribeTextStyleObjType);
  sub_821241D0(style, base);
  const uint32_t frame = engine::ScratchFrame(ctx.r1.u32);
  REX_STORE_U16(frame + 0x56, 0x120);
  REX_STORE_U16(frame + 0x5E, 0x1B0);
  REX_STORE_U16(frame + 0x66, 0);
  PPCContext call = ctx;
  call.r1.u64 = frame;
  call.r3.u64 = text;
  call.r4.u64 = style.r3.u32;
  call.f1.f64 = 0.0;
  call.r6.u64 = 1;
  call.r7.u64 = 0;
  call.r8.u64 = kTribeMenuSelected;
  call.r9.u64 = 0x73;
  call.r10.u64 = 0x20D;
  sub_821068C0(call, base);
}

void SetConfirmMenu(uint8_t* base, uint32_t window) {
  const uint32_t menu = MenuOf(base, TextObjectOf(base, window));
  REX_STORE_U32(menu + kMenuChangedCallback, 0);
  REX_STORE_U32(menu + kMenuCallbackData, kConfirmPage);
  REX_STORE_U32(menu + kMenuBackCallback, 0);
  REX_STORE_U16(menu, REX_LOAD_U16(menu) | 0x80);
}

uint32_t Confirm(PPCContext& ctx, uint8_t* base, uint32_t tribe, const ExtraRow& row) {
  if (engine::BankedScore(base) < row.price) {
    if (const uint32_t text = GuestWideString(u"You'll need a higher score than that.")) AddSpeech(ctx, base, tribe, text);
    return 1;
  }
  const uint32_t text = GuestWideString(u"Buy this item for " + Number(int32_t(row.price), u" points?"));
  if (!text) return 1;
  AddSpeech(ctx, base, tribe, text);
  PPCContext game_text = ctx;
  game_text.r3.u64 = tribe;
  sub_82106AB8(game_text, base);
  const uint32_t window = game_text.r3.u32;
  if (!window) return 1;
  SetConfirmMenu(base, window);
  const uint32_t prefix = LocalisedString(ctx, base, kRowPrefixString);
  AddTextItem(ctx, base, window, 0, prefix, LocalisedString(ctx, base, kYesString), kTribeMenuSelected, true);
  AddTextItem(ctx, base, window, 0, prefix, LocalisedString(ctx, base, kNoString), kTribeMenuSelected, true);
  SelectRow(ctx, base, window, 1);
  SetConfirmMenu(base, window);
  return 1;
}

void Buy(PPCContext& ctx, uint8_t* base, const ExtraRow& row) {
  if (engine::BankedScore(base) < row.price) return;
  engine::SpendBankedScore(base, row.price);
  engine::GrantCollectible(ctx, base, *row.next);
  CallGuest(ctx, base, sub_822EA728, kPurchaseSound);
  if (row.kind->preview) CallGuest(ctx, base, sub_82251DF8, REX_LOAD_U32(kCurrentTribe), 0, kMsgSendPreviewOff);
  REXLOG_INFO("[kameo] bought back collectible event {:#x} for {} points", row.next->event, row.price);
}

}  // namespace

}  // namespace kameo::ui

using namespace kameo::ui;

// tribeShopRender(tribe, window, lastResult, page, selectedRow)
extern "C" REX_FUNC(sub_82139C20) {
  const uint32_t tribe = ctx.r3.u32;
  const uint32_t page = ctx.r6.u32;
  const uint32_t own_rows = OwnRowCount();
  const std::vector<ExtraRow> rows = ExtraRows(ctx, base);
  uint32_t slot = REX_LOAD_U32(kShopSelectedSlot);
  if (slot >= own_rows && slot - own_rows < rows.size() && (page == kConfirmPage || page == kBuyPage)) {
    const ExtraRow& row = rows[slot - own_rows];
    PPCContext clear = ctx;
    clear.r6.u64 = kClearOnlyPage;
    __imp__sub_82139C20(clear, base);
    if (page == kConfirmPage) {
      ctx.r3.u64 = Confirm(ctx, base, tribe, row);
      return;
    }
    if ((ctx.r7.u32 & 0xFF) == 0) Buy(ctx, base, row);
    ctx.r6.u64 = kListPage;
    sub_82139C20(ctx, base);
    return;
  }
  const bool lists = page == kListPage || page == kBuyPage;
  if (!lists || rows.empty()) {
    __imp__sub_82139C20(ctx, base);
    return;
  }
  // The game scrolls to and previews the remembered row, so one of ours is held back until our rows exist.
  const bool ours = slot >= own_rows;
  if (ours) REX_STORE_U32(kShopSelectedSlot, 0);
  __imp__sub_82139C20(ctx, base);
  const uint32_t result = ctx.r3.u32;
  AddExtraRows(ctx, base, rows);
  if (ours) {
    slot = std::min<uint32_t>(slot, own_rows + uint32_t(rows.size()) - 1);
    REX_STORE_U32(kShopSelectedSlot, slot);
    if (const uint32_t window = MenuWindow(ctx, base)) {
      SelectRow(ctx, base, window, slot);
      CallGuest(ctx, base, sub_821398C8, window, slot, kListPage);
    }
  }
  ctx.r3.u64 = result;
}

// _tribeShopItemChanged(window, row, page)
extern "C" REX_FUNC(sub_821398C8) {
  const uint32_t row = ctx.r4.u32 & 0xFF;
  const uint32_t own_rows = OwnRowCount();
  if (ctx.r5.u32 != kListPage || row < own_rows) {
    __imp__sub_821398C8(ctx, base);
    return;
  }
  const std::vector<ExtraRow> rows = ExtraRows(ctx, base);
  REX_STORE_U32(kShopSelectedSlot, row);
  ShowPreview(ctx, base, row - own_rows < rows.size() ? rows[row - own_rows].kind : nullptr);
}
