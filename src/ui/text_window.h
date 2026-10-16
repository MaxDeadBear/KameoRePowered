// Helpers for the game's text windows (the boxes every menu, prompt and HUD label is drawn in).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "core/guest.h"
#include "input/guest_controls.h"

namespace kameo::ui {

// A text window's text object.
constexpr uint32_t kWindowTextObject = 16;

constexpr uint32_t kTextWindow = 48;
constexpr uint32_t kTextFirstItem = 124;
constexpr uint32_t kTextLeft = 130;
constexpr uint32_t kTextRight = 132;
constexpr uint32_t kTextTop = 134;
constexpr uint32_t kTextBottom = 136;
constexpr uint32_t kTextMargin = 138;
constexpr uint32_t kTextFont = 140;
constexpr uint32_t kTextColourCallback = 176;
constexpr uint32_t kTextColourCallbackData = 180;
constexpr uint32_t kTextMenu = 256;

// One line of a text object's item list.
constexpr uint32_t kItemNext = 4;
constexpr uint32_t kItemSelectable = 8;
constexpr uint32_t kItemFormattedText = 12;
constexpr uint32_t kItemLineCount = 16;
constexpr uint32_t kItemCentred = 28;
constexpr uint32_t kItemHeight = 108;
constexpr uint32_t kItemTop = 128;
constexpr uint32_t kItemCallback = 132;
constexpr uint32_t kItemOverwrite = 137;
constexpr uint32_t kItemText = 140;
constexpr uint32_t kItemId = 144;

// The menu record at text_object+256.
constexpr uint16_t kMenuFlagScrolling = 0x10;
constexpr uint32_t kMenuScrollStart = 84;
constexpr uint32_t kMenuScroll = 88;
constexpr uint32_t kMenuScrollTarget = 92;
constexpr uint32_t kMenuBackCallback = 100;
constexpr uint32_t kMenuChangedCallback = 104;
constexpr uint32_t kMenuCallbackData = 108;
constexpr uint32_t kMenuPreviousIndex = 112;
constexpr uint32_t kMenuSelectedIndex = 114;
constexpr uint32_t kMenuMoveSound = 132;
constexpr uint32_t kMenuSelectSound = 136;
constexpr uint32_t kMenuBackSound = 140;

// The game's default menu highlight colour callback.
constexpr uint32_t kTextDefaultMenuColourCallback = 0x82106770;

using kameo::input::LoadFloat;
using kameo::input::StoreFloat;

inline uint32_t TextObjectOf(const uint8_t* base, uint32_t window) { return REX_LOAD_U32(window + kWindowTextObject); }
inline uint32_t MenuOf(const uint8_t* base, uint32_t text_object) { return REX_LOAD_U32(text_object + kTextMenu); }

// A NUL-terminated UTF-16 copy of `text` in guest memory, allocated once per distinct string and never freed.
uint32_t GuestWideString(const std::u16string& text);
std::u16string Number(int32_t value, const char16_t* suffix);
void CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*), uint64_t r3, uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0);

uint32_t LocalisedString(PPCContext& ctx, uint8_t* base, int32_t id);
// The game's own line with its visible text swapped for `label`, keeping its {font control tags} such as centring.
uint32_t StyledLike(uint8_t* base, uint32_t game_text, const std::u16string& label);
// Only the {tags} ahead of a game line's first visible character, so its centring and style carry over but inline glyph tags later in the line do not.
uint32_t WithLeadingTags(uint8_t* base, uint32_t game_text, const std::u16string& label);
// Reads a guest wide string without its {tag} formatting.
std::u16string PlainGuestText(uint8_t* base, uint32_t text);
std::u16string SizeTag(int size);

// Set while one of our own lines is being added, so the add-line hook lets it through untouched.
extern thread_local bool t_addingOwnTextItem;
void AddTextItem(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t id, uint32_t format, uint32_t text, uint32_t callback, bool selectable);

// The lowest edge any line of the window reaches, in layout units.
float TextContentBottom(uint8_t* base, uint32_t text_object);
// The selectable item at `index` in the window's text list, as the game's own A-press callback counts them.
uint32_t SelectedItemId(uint8_t* base, uint32_t window, uint32_t index);
uint32_t SelectedIndex(uint8_t* base, uint32_t window);
uint32_t SelectableItem(uint8_t* base, uint32_t text_object, int32_t index);
void KeepCursor(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t index);
// Where a line sits vertically in the 640x480 text space, or -1 when the window has no line with that id.
float TextLineCentre(uint8_t* base, uint32_t text_object, uint32_t line_id);

void PlayMenuSound(PPCContext& ctx, uint8_t* base, uint32_t sound);
void CallMenuCallback(PPCContext& ctx, uint8_t* base, uint32_t callback, uint32_t window, uint32_t index, uint32_t data);
// Moves the cursor the way textWindow_tick does for the stick: previous index, the changed callback and the move sound.
void SelectMenuItem(PPCContext& ctx, uint8_t* base, uint32_t text_object, int32_t index, bool scroll);

}  // namespace kameo::ui
