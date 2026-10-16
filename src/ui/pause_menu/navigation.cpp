// A and B on the pause and options pages: opening our pages, walking back up them, and opening the main menu's options on the category list.

#include "app/process.h"
#include "core/settings.h"
#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;

namespace {

thread_local bool t_openingOptionsObject = false;

}  // namespace

// dc_matchStoryBookSimpleOptionsObjName(window, _, pauseObj) -- the settings pages' B handler; on a category page it returns to that page's list instead of the pause menu.
extern "C" REX_FUNC(sub_823F1028) {
  const uint32_t window = ctx.r3.u32;
  const uint32_t pause = ctx.r5.u32;
  if (g_currentPage == Page::ConfirmReset) {
    ReturnFromReset(ctx, base, pause, window);
    return;
  }
  if (BackFromLevelSelect(ctx, base, pause, window, g_currentPage)) return;
  if (BackFromCollectibles(ctx, base, pause, window, g_currentPage)) return;
  const PageInfo* info = FindPage(g_currentPage);
  if (info && info->parent != Page::None) {
    const Page returning_from = info->page;
    ActivatePage(ctx, base, pause, window, info->parent);
    KeepCursor(ctx, base, window, CategoryIndex(returning_from));
    return;
  }
  // B from a category or the game's settings page goes back to the Options list; B there returns to the pause menu's main page, or closes the main menu's options.
  if (g_currentPage != Page::Options && (g_currentPage != Page::None || IsOptionsObject(base, pause))) {
    const Page returning_from = g_currentPage;
    ActivatePage(ctx, base, pause, window, Page::Options);
    KeepCursor(ctx, base, window, OptionsCategoryIndex(returning_from));
    return;
  }
  g_currentPage = Page::None;
  __imp__sub_823F1028(ctx, base);
}

// storybookSimplePauseTextReady(window, pauseObj) -- the options object opens on its settings page from here once both windows exist.
extern "C" REX_FUNC(sub_823EF5B8) {
  t_openingOptionsObject = IsOptionsObject(base, ctx.r4.u32);
  __imp__sub_823EF5B8(ctx, base);
  t_openingOptionsObject = false;
}

// activateSimplePauseSettingsMenu(pauseObj, window) -- the main menu's options open on the category list instead of the game's settings page, which becomes its first line.
extern "C" REX_FUNC(sub_823EFF38) {
  if (!t_openingOptionsObject || !ctx.r4.u32) {
    __imp__sub_823EFF38(ctx, base);
    return;
  }
  t_openingOptionsObject = false;
  g_brightnessFromDisplay = false;
  ActivatePage(ctx, base, ctx.r3.u32, ctx.r4.u32, Page::Options);
}

// storybookSimplePauseMenuPressACallback(window, index, pauseObj)
extern "C" REX_FUNC(sub_823F1078) {
  const uint32_t window = ctx.r3.u32;
  const uint32_t pause = ctx.r5.u32;
  const uint32_t index = ctx.r4.u32 & 0xFF;
  const uint32_t id = SelectedItemId(base, window, index);
  if (id == kItemResume) {
    g_resumeRequested = true;
    return;
  }
  if (id == kItemQuitApplication) {
    KameoSaveSettings();
    KameoQuitApplication();
    return;
  }
  if (id == kItemRestartGame) {
    KameoSaveSettings();
    KameoRestartApplication();
    return;
  }
  if (const PageInfo* info = FindPageByItem(id)) {
    ActivatePage(ctx, base, pause, window, info->page);
    return;
  }
  if (id == kItemOptionsPage) {
    ActivatePage(ctx, base, pause, window, Page::Options);
    return;
  }
  if (PressLevelSelectItem(ctx, base, pause, window, id)) return;
  if (PressCollectibleItem(ctx, base, pause, window, id)) return;
  if (id == kItemPlayCredits) {
    g_playCreditsRequested = true;
    g_resumeRequested = true;
    return;
  }
  if (PressRestoreDefaultsItem(ctx, base, pause, window, id, index)) return;
  if (id == kItemBrightness) {
    g_brightnessFromDisplay = true;
    g_brightnessIndex = index;
    g_currentPage = Page::None;
    PPCContext call = ctx;
    call.r3.u64 = pause;
    call.r4.u64 = window;
    sub_823EFFD0(call, base);
    return;
  }
  if (const Option* option = FindOption(id)) {
    ChangeOption(ctx, base, pause, window, *option, 1, true);
    return;
  }
  __imp__sub_823F1078(ctx, base);
}
