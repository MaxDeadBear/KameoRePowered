// The quit prompt: a restart line under the game's own, or with keyboard and mouse, pointer-friendly Quit, Restart and Back lines.

#include <algorithm>
#include <cmath>
#include <string>

#include "input/input.h"
#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;

namespace {

// The quit prompt's box grows to fit the extra line, evenly above and below, and is put back when any other page is built.
struct QuitWindowSize {
  uint32_t text_object = 0;
  int16_t top = 0;
  int16_t bottom = 0;
};
QuitWindowSize g_quitWindowSize;

void FitQuitWindow(uint8_t* base, uint32_t text_object) {
  RestoreQuitWindow(base);
  const int16_t top = int16_t(REX_LOAD_U16(text_object + kTextTop));
  const int16_t bottom = int16_t(REX_LOAD_U16(text_object + kTextBottom));
  const int32_t overflow = int32_t(std::ceil(TextContentBottom(base, text_object))) - (bottom - top);
  if (overflow <= 0) return;
  g_quitWindowSize = {text_object, top, bottom};
  const int32_t above = overflow / 2;
  REX_STORE_U16(text_object + kTextTop, uint16_t(top - above));
  REX_STORE_U16(text_object + kTextBottom, uint16_t(bottom + (overflow - above)));
  // Adding the line overflowed the old height and switched the page to scrolling, which the larger box makes unnecessary.
  const uint32_t menu = REX_LOAD_U32(text_object + kTextMenu);
  REX_STORE_U16(menu, uint16_t(REX_LOAD_U16(menu) & ~kMenuFlagScrolling));
  for (uint32_t field : {84u, 88u, 92u}) REX_STORE_U32(menu + field, 0);
}

}  // namespace

void kameo::ui::RestoreQuitWindow(uint8_t* base) {
  if (!g_quitWindowSize.text_object) return;
  REX_STORE_U16(g_quitWindowSize.text_object + kTextTop, uint16_t(g_quitWindowSize.top));
  REX_STORE_U16(g_quitWindowSize.text_object + kTextBottom, uint16_t(g_quitWindowSize.bottom));
  g_quitWindowSize.text_object = 0;
}

// setupMenuItemsForQuitMenu(window) -- adds the restart line under the game's own quit prompt, with the formatting its last line starts with.
extern "C" REX_FUNC(sub_823F1690) {
  g_currentPage = Page::None;
  const uint32_t window = ctx.r3.u32;
  __imp__sub_823F1690(ctx, base);
  if (!window) return;
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  const uint32_t title_item = REX_LOAD_U32(text_object + kTextFirstItem);
  const uint32_t caution_item = title_item ? REX_LOAD_U32(title_item + kItemNext) : 0;
  // A click is an A press, and A anywhere on the game's prompt quits, so with keyboard and mouse its "Press A / Press B" lines become Quit, Restart Game and Back to point at.
  if (KameoKeyboardMouseMode() && caution_item) {
    const uint32_t title = REX_LOAD_U32(title_item + kItemText);
    std::u16string caution;
    for (uint32_t i = 0, text = REX_LOAD_U32(caution_item + kItemText); text && i < 1024; ++i) {
      const char16_t c = char16_t(REX_LOAD_U16(text + i * 2));
      if (!c) break;
      caution.push_back(c);
    }
    if (const size_t prompt = caution.find(u"\n\nPress"); prompt != std::u16string::npos) caution.resize(prompt);
    PPCContext clear = ctx;
    clear.r3.u64 = text_object;
    sub_82107510(clear, base);
    // Each line copies the window's style block (text_object+148 onward) when it is added, so the highlight colour callback the game left empty on this page has to be in place first.
    REX_STORE_U32(text_object + kTextColourCallback, kTextDefaultMenuColourCallback);
    REX_STORE_U32(text_object + kTextColourCallbackData, 0);
    AddTextItem(ctx, base, window, 0, 0, title, 0, false);
    if (const uint32_t text = GuestWideString(caution + u"\n")) AddTextItem(ctx, base, window, 0, 0, text, 0, false);
    constexpr const char16_t* kLineStyle = u"{alignment='centre' size='35'}";
    if (const uint32_t text = GuestWideString(std::u16string(kLineStyle) + u"Quit to Menu")) AddTextItem(ctx, base, window, kItemQuitConfirm, 0, text, kReturnToLobbyCallback, true);
    if (const uint32_t text = GuestWideString(std::u16string(kLineStyle) + u"Quit Application")) AddTextItem(ctx, base, window, kItemQuitApplication, 0, text, kPauseMenuPressACallback, true);
    if (const uint32_t text = GuestWideString(std::u16string(kLineStyle) + u"Restart Game")) AddTextItem(ctx, base, window, kItemRestartGame, 0, text, kPauseMenuPressACallback, true);
    if (const uint32_t text = GuestWideString(std::u16string(kLineStyle) + u"Back")) AddTextItem(ctx, base, window, kItemQuitBack, 0, text, kQuitBackCallback, true);
    // The cursor starts on Back so a stray click never quits.
    REX_STORE_U8(REX_LOAD_U32(text_object + kTextMenu) + kMenuSelectedIndex, 3);
    FitQuitWindow(base, text_object);
    return;
  }
  uint32_t last = 0;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) last = item;
  // The leading newline repeats the blank line the game puts between its own prompts; U+E002 is the X glyph characterButtonCallback draws after A (E000) and B (E001).
  if (const uint32_t text = WithLeadingTags(base, last ? REX_LOAD_U32(last + kItemText) : 0, u"\nPress \uE002 to restart the game\n\nPress \uE003 to quit the application")) AddTextItem(ctx, base, window, 0, 0, text, 0, false);
  FitQuitWindow(base, text_object);
}
