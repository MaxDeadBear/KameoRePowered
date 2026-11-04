// The pause menu's pages: which categories exist, and building a page's lines into the game's settings window.

#include <algorithm>

#include "core/localization.h"
#include "core/settings.h"
#include "core/unlock_codes.h"
#include "ui/pause_menu/option_values.h"
#include "ui/pause_menu/pause_menu_internal.h"

namespace kameo::ui {

Page g_currentPage = Page::None;
uint32_t g_menuPlayer = 0;
bool g_brightnessFromDisplay = false;
uint32_t g_brightnessIndex = 0;
bool g_resumeRequested = false;
bool g_playCreditsRequested = false;
bool g_levelSelectAvailable = false;
Page g_hotkeyPage = Page::None;

namespace {

const PageInfo kPages[] = {
    {Page::Video, Page::None, TR_NOOP(u"Display"), kItemVideoPage},
    {Page::Graphics, Page::None, TR_NOOP(u"Graphics"), 0x7A5E0302},
    {Page::Audio, Page::None, TR_NOOP(u"Audio"), kItemAudioPage},
    {Page::Controls, Page::None, TR_NOOP(u"Controls"), kItemControlsPage},
    {Page::Coop, Page::None, TR_NOOP(u"Split-screen"), 0x7A5E030D},
    {Page::Gameplay, Page::None, TR_NOOP(u"Gameplay"), 0x7A5E0308},
    {Page::Mods, Page::None, TR_NOOP(u"Mods"), kItemModsPage},
    {Page::Achievements, Page::None, TR_NOOP(u"Achievements"), kItemAchievementsPage},
    {Page::Hud, Page::Video, TR_NOOP(u"HUD"), 0x7A5E030B},
    {Page::Effects, Page::Graphics, TR_NOOP(u"Effects"), 0x7A5E0303},
    {Page::Materials, Page::Graphics, TR_NOOP(u"Materials"), 0x7A5E030C},
    {Page::Camera, Page::Controls, TR_NOOP(u"Camera"), 0x7A5E030A},
    {Page::Bindings, Page::Controls, TR_NOOP(u"Key Bindings"), 0x7A5E0310},
    {Page::PadBindings, Page::Controls, TR_NOOP(u"Controller Bindings"), 0x7A5E0311},
};

// The Options page, the pause menu's and the main menu's alike: these categories, then Credits in a game, and Restore All Defaults.
constexpr Page kOptionsCategories[] = {Page::Video, Page::Graphics, Page::Audio, Page::Controls, Page::Coop, Page::Gameplay, Page::Mods, Page::Achievements};

void AddPageRows(PPCContext& ctx, uint8_t* base, uint32_t window, Page page) {
  if (AddLevelSelectRows(ctx, base, window, page)) return;
  if (AddCollectibleRows(ctx, base, window, page)) return;
  if (AddAchievementRows(ctx, base, window, page)) return;
  if (page == Page::Options) {
    AddPageTitle(ctx, base, window, kameo::l10n::Tr(u"Options"));
    for (const Page category : kOptionsCategories) AddCategoryLine(ctx, base, window, FindPage(category)->item, kameo::l10n::Tr(FindPage(category)->title));
    if (g_levelSelectAvailable) AddCategoryLine(ctx, base, window, kItemPlayCredits, kameo::l10n::Tr(u"Credits"));
    AddCategoryLine(ctx, base, window, kItemRestoreAllDefaults, kameo::l10n::Tr(u"Restore All Defaults"));
    return;
  }
  if (page == Page::ConfirmReset) {
    AddConfirmResetRows(ctx, base, window);
    return;
  }
  const PageInfo* info = FindPage(page);
  AddPageTitle(ctx, base, window, info ? kameo::l10n::Tr(info->title) : std::u16string());
  // The game's brightness and gamma page opens from the end of Display, and a page's sub-categories follow its own options.
  for (const Option& option : AllOptions()) {
    if (option.page != page || (option.shown && !option.shown())) continue;
    AddToggleRow(ctx, base, window, option.id, kameo::l10n::Tr(option.label), option.game_value ? LocalisedString(ctx, base, option.game_value()) : GuestWideString(option.value()));
  }
  // Beside settings, a line that opens another page is a row of the same size marked with an arrow.
  const bool settings_page = VisibleOptionCount(page) > 0;
  auto add_link = [&](uint32_t id, const std::u16string& label) {
    if (settings_page) AddToggleRow(ctx, base, window, id, label, GuestWideString(u">"));
    else AddCategoryLine(ctx, base, window, id, label);
  };
  if (page == Page::Video) add_link(kItemBrightness, kameo::l10n::Tr(u"Brightness & Gamma"));
  if (page == Page::Gameplay && g_levelSelectAvailable && KameoCodeUnlocked(KameoUnlockCode::LevelSelect)) add_link(kItemLevelSelectPage, kameo::l10n::Tr(u"Level Select"));
  if (page == Page::Gameplay && g_levelSelectAvailable && KameoCodeUnlocked(KameoUnlockCode::CollectibleTracker)) add_link(kItemCollectiblesPage, kameo::l10n::Tr(u"Collectibles"));
  for (const PageInfo& child : kPages) {
    if (child.parent == page) add_link(child.item, kameo::l10n::Tr(child.title));
  }
  if (HasDefaults(page)) add_link(kItemRestoreDefaults, kameo::l10n::Tr(u"Restore Defaults"));
}

}  // namespace

void AddToggleRow(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t id, const std::u16string& label, uint32_t value_text) {
  const uint32_t label_text = GuestWideString(label);
  if (!label_text || !value_text) return;
  AddTextItem(ctx, base, window, 0, LocalisedString(ctx, base, kLabelFormatString), label_text, 0, false);
  REX_STORE_U8(kNextTextItemWriteOverPreviousLine, 1);
  AddTextItem(ctx, base, window, id, LocalisedString(ctx, base, kValueFormatString), value_text, kPauseMenuPressACallback, true);
}

std::span<const PageInfo> AllPages() { return kPages; }
std::span<const Page> OptionsCategories() { return kOptionsCategories; }

const PageInfo* FindPage(Page page) {
  for (const PageInfo& info : kPages) {
    if (info.page == page) return &info;
  }
  return nullptr;
}

const PageInfo* FindPageByItem(uint32_t item) {
  for (const PageInfo& info : kPages) {
    if (info.item == item) return &info;
  }
  return nullptr;
}

uint32_t CategoryIndex(Page page) {
  const PageInfo* info = FindPage(page);
  uint32_t index = 0;
  // Categories follow the parent's own options, and Video's Brightness & Gamma row.
  for (const Option& option : AllOptions()) {
    if (info && option.page == info->parent && (!option.shown || option.shown())) ++index;
  }
  if (info && info->parent == Page::Video) ++index;
  for (const PageInfo& sibling : kPages) {
    if (&sibling == info) return index;
    if (info && sibling.parent == info->parent) ++index;
  }
  return 0;
}

uint32_t OptionsCategoryIndex(Page page) {
  for (uint32_t i = 0; i < std::size(kOptionsCategories); ++i) {
    if (kOptionsCategories[i] == page) return i;
  }
  return uint32_t(std::size(kOptionsCategories));
}

void AddPageTitle(PPCContext& ctx, uint8_t* base, uint32_t window, const std::u16string& title) {
  if (const uint32_t text = StyledLike(base, LocalisedString(ctx, base, kGameOptionsTitleString), title)) AddTextItem(ctx, base, window, 0, 0, text, 0, false);
}

void AddCategoryLine(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t id, const std::u16string& label) {
  if (const uint32_t text = StyledLike(base, LocalisedString(ctx, base, kGameOptionsLineString), label)) AddTextItem(ctx, base, window, id, 0, text, kPauseMenuPressACallback, true);
}

void ActivatePage(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, Page page) {
  RestoreQuitWindow(base);
  const uint32_t text_object = TextObjectOf(base, window);
  const uint32_t menu = MenuOf(base, text_object);
  for (uint32_t field : {144u, kMenuScroll, kMenuScrollTarget, kMenuChangedCallback, kMenuScrollStart}) REX_STORE_U32(menu + field, 0);
  REX_STORE_U32(menu + kMenuBackCallback, kSettingsBackCallback);
  REX_STORE_U32(menu + kMenuCallbackData, pause);
  REX_STORE_U8(menu + kMenuSelectedIndex, 0);
  REX_STORE_U16(menu + kMenuPreviousIndex, 0xFFFF);
  REX_STORE_U16(menu, uint16_t(REX_LOAD_U16(menu) & 0xA));
  PPCContext clear = ctx;
  clear.r3.u64 = text_object;
  sub_82107510(clear, base);
  REX_STORE_U32(text_object + kTextColourCallback, kTextDefaultMenuColourCallback);
  REX_STORE_U32(text_object + kTextColourCallbackData, 0);
  g_levelSelectAvailable = !IsOptionsObject(base, pause);
  g_menuPlayer = uint32_t(std::clamp<int8_t>(int8_t(REX_LOAD_U8(REX_LOAD_U32(pause + 8) + 146)), 0, 3));
  AddPageRows(ctx, base, window, page);
  g_currentPage = page;
  NoteActivatedPage(page, text_object);
  REX_STORE_U32(REX_LOAD_U32(pause + 512) + 120, 1);
}

void ApplyOption(PPCContext& ctx, uint8_t* base, uint32_t pause, const Option& option, int direction, bool wrap) {
  option.step(direction, wrap);
  if (option.id == kItemHints) {
    PPCContext hint = ctx;
    sub_8226ADE0(hint, base);
    REX_STORE_U32(kCurrentHint, hint.r3.u32);
  }
  // The game marks its settings changed at +64 of the pause record, as its own Game Options page does, so they are saved with the profile.
  if (option.game_value && pause && REX_LOAD_U32(pause + 512)) REX_STORE_U32(REX_LOAD_U32(pause + 512) + 64, 1);
  KameoSaveSettings();
}

void ChangeOption(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, const Option& option, int direction, bool wrap) {
  const uint32_t index = SelectedIndex(base, window);
  ApplyOption(ctx, base, pause, option, direction, wrap);
  // The gamma bar is drawn from the setting every frame, like the brightness bar beside it, so its page needs no rebuild.
  if (option.page == Page::GameBrightness) return;
  ActivatePage(ctx, base, pause, window, option.page);
  KeepCursor(ctx, base, window, index);
}

bool IsOptionsObject(uint8_t* base, uint32_t pause) {
  const uint32_t header = pause ? REX_LOAD_U32(pause + 8) : 0;
  if (!header) return false;
  constexpr const char kName[] = "StoryBookSimpleOptionsObj";
  // The object's name; the title update's object record has four more bytes ahead of it.
#if KAMEO_TU
  constexpr uint32_t kObjectName = 196;
#else
  constexpr uint32_t kObjectName = 192;
#endif
  for (uint32_t i = 0; i < sizeof(kName); ++i) {
    if (REX_LOAD_U8(header + kObjectName + i) != uint8_t(kName[i])) return false;
  }
  return true;
}

}  // namespace kameo::ui
