// Where our lines go into the game's own pause pages as it builds them, and the scroll fix that keeps headings in view.

#include <algorithm>

#include "ui/hud/key_labels.h"
#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;

namespace {

// Set while the game builds one of its pages, so the add-line hook knows where our lines go.
thread_local bool t_building_main_page = false;
thread_local bool t_building_settings_page = false;
thread_local bool t_building_gamma_page = false;

// Game Options' camera inversions, HUD settings, split-screen layout and hints, which live on our pages instead.
constexpr uint32_t kMovedGameItems[] = {4806887, 196833303, 254820151, 86359684, 5351767, 108173431, 165738686, 255437843};
constexpr int32_t kMovedGameLabels[] = {4453, 4454, 4456, 4457, 4463, 4459, 4460};

}  // namespace

// xfix_buildLocalisedMessageString(window, id, ...) -- our main-page lines go in ahead of Quit, styled like it.
extern "C" REX_FUNC(sub_8210A4C8) {
  if (const uint32_t text = KameoIconLabelText(base, ctx.r3.u32, ctx.r6.u32)) ctx.r6.u64 = text;
  KameoWidenPromptBar(base, ctx.r3.u32, ctx.r6.u32);
  KameoWidenPromptBar(base, ctx.r3.u32, ctx.r5.u32);
  // The game's brightness bar line has no text of its own, so it gains a Brightness label like the Gamma bar below it.
  if (!t_addingOwnTextItem && t_building_gamma_page && ctx.r4.u32 == kGameItemBrightnessBar) {
    t_building_gamma_page = false;
    const uint32_t label_format = LocalisedString(ctx, base, kLabelFormatString);
    if (const uint32_t label = GuestWideString(u"Brightness")) AddTextItem(ctx, base, ctx.r3.u32, kItemBrightnessLabel, label_format, label, 0, false);
  }
  if (!t_addingOwnTextItem && t_building_settings_page && ctx.r4.u32 == kGameItemBrightness) {
    ctx.r3.u64 = 0;
    return;
  }
  // The moved settings' label and value lines leave Game Options; the value line was to write over its label, which is cleared with it.
  if (!t_addingOwnTextItem && t_building_settings_page) {
    if (std::find(std::begin(kMovedGameItems), std::end(kMovedGameItems), ctx.r4.u32) != std::end(kMovedGameItems)) {
      REX_STORE_U8(kNextTextItemWriteOverPreviousLine, 0);
      ctx.r3.u64 = 0;
      return;
    }
    if (ctx.r4.u32 == 0 && ctx.r6.u32 && std::any_of(std::begin(kMovedGameLabels), std::end(kMovedGameLabels), [&](int32_t label) { return LocalisedString(ctx, base, label) == ctx.r6.u32; })) {
      ctx.r3.u64 = 0;
      return;
    }
  }
  // The main page is Resume, Options and Quit: Game Options' remaining settings are reached from Options.
  if (!t_addingOwnTextItem && t_building_main_page && ctx.r4.u32 == kGameItemGameOptions) {
    if (const uint32_t text = StyledLike(base, ctx.r6.u32, u"Resume")) AddTextItem(ctx, base, ctx.r3.u32, kItemResume, ctx.r5.u32, text, kPauseMenuPressACallback, true);
    ctx.r3.u64 = 0;
    return;
  }
  if (!t_addingOwnTextItem && t_building_main_page && ctx.r4.u32 == kGameItemQuit) {
    t_building_main_page = false;
    const uint32_t window = ctx.r3.u32;
    const uint32_t quit_text = ctx.r6.u32;
    if (const uint32_t text = StyledLike(base, quit_text, u"Options")) AddTextItem(ctx, base, window, kItemOptionsPage, 0, text, kPauseMenuPressACallback, true);
  }
  __imp__sub_8210A4C8(ctx, base);
}

// textWindowScrollToMenuItem(window, index, seconds) -- the game scrolls the selected line to the top, so a heading above the first line never scrolls back into view.
extern "C" REX_FUNC(sub_8210ACA0) {
  const uint32_t window = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32 & 0xFF;
  const float seconds = float(ctx.f1.f64);
  __imp__sub_8210ACA0(ctx, base);
  if (!window) return;
  const uint32_t text_object = TextObjectOf(base, window);
  const uint32_t menu = MenuOf(base, text_object);
  uint32_t heading = 0;
  uint32_t seen = 0;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemSelectable) != 1) {
      if (!heading) heading = item;
      continue;
    }
    if (seen++ != index) {
      heading = 0;
      continue;
    }
    if (!heading || !menu) return;
    const float top = std::max(0.0f, LoadFloat(base, heading + kItemTop));
    const float window_height = float(int16_t(REX_LOAD_U16(text_object + kTextBottom)) - int16_t(REX_LOAD_U16(text_object + kTextTop)));
    if (top >= LoadFloat(base, menu + kMenuScrollTarget) || LoadFloat(base, item + kItemTop) + LoadFloat(base, item + kItemHeight) - top > window_height) return;
    StoreFloat(base, menu + kMenuScrollTarget, top);
    if (seconds == 0.0f) {
      StoreFloat(base, menu + kMenuScrollStart, top);
      StoreFloat(base, menu + kMenuScroll, top);
    }
    return;
  }
}

// xfixgem_pauseMenu_setupButtonPrompts(window) -- builds the main pause page.
extern "C" REX_FUNC(sub_823F0068) {
  g_currentPage = Page::None;
  g_brightnessFromDisplay = false;
  RestoreQuitWindow(base);
  t_building_main_page = true;
  __imp__sub_823F0068(ctx, base);
  t_building_main_page = false;
}

// setupMenuItemsForSettingsMenu(window, pauseObj) -- the game's own Game Options page, so B there is the game's again.
extern "C" REX_FUNC(sub_823F0240) {
  g_currentPage = Page::None;
  g_brightnessFromDisplay = false;
  t_building_settings_page = true;
  __imp__sub_823F0240(ctx, base);
  t_building_settings_page = false;
}

// xfixgem_pauseMenu_adjustGammaPreset(window) -- builds the game's brightness page, which gains the display Gamma line.
extern "C" REX_FUNC(sub_823F0CC0) {
  const uint32_t window = ctx.r3.u32;
  t_building_gamma_page = true;
  __imp__sub_823F0CC0(ctx, base);
  t_building_gamma_page = false;
  if (!window) return;
  // Each bar is about two lines tall, so a spacer follows it; then a Gamma label and an empty selectable line, the same pair the game uses for its own bar.
  const uint32_t label_format = LocalisedString(ctx, base, kLabelFormatString);
  const uint32_t empty = GuestWideString(u"");
  const uint32_t label = GuestWideString(u"Gamma");
  if (!empty || !label) return;
  AddTextItem(ctx, base, window, kItemBarSpacer, label_format, empty, 0, false);
  AddTextItem(ctx, base, window, kItemGammaLabel, label_format, label, 0, false);
  AddTextItem(ctx, base, window, kItemGammaBar, label_format, empty, kPauseMenuPressACallback, true);
  AddTextItem(ctx, base, window, kItemBarSpacer, label_format, empty, 0, false);
}
