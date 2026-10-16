// The in-game name entry that answers the profile screen's call to the system keyboard, and X Rename on the save list.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "input/guest_controls.h"
#include "input/input.h"
#include "ui/name_entry/name_entry.h"
#include "ui/name_entry/save_rename.h"
#include "ui/text_window.h"

using namespace kameo::ui;
using namespace kameo::input;

namespace {

// Typed into from the controller's letter grid or a physical keyboard.
constexpr uint32_t kNameEntryMaxLength = 12;
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorCancelled = 1223;
// The profile screen shows its popup window (record+16) while this event is clear, as createErrorMessage does.
constexpr uint32_t kProfilePopupHiddenEvent = 0x1E86AB;
// Profile screen states on record+24: 1 reloads the saves, 3 is the save list; 22 is unused by the game and parks it while a save is renamed.
constexpr uint32_t kProfileStateReload = 1;
constexpr uint32_t kProfileStateList = 3;
constexpr uint32_t kProfileStateRenaming = 22;
// The save list item id canDeleteCurrSelection accepts, i.e. an existing save.
constexpr uint32_t kSaveSlotItemId = 147498740;

struct NameEntry {
  bool active = false;
  bool renaming = false;
  std::string file_name;
  uint32_t owner = 0;
  uint32_t window = 0;
  uint32_t result_buffer = 0;
  uint32_t result_capacity = 0;
  uint32_t overlapped = 0;
  uint32_t title = 0;
  std::u16string text;
  int32_t row = 0;
  int32_t column = 0;
  bool lower_case = false;
  bool controller_ready = false;
  float blink = 0.0f;
  bool cursor_on = true;
  int32_t held_direction = 0;
  float repeat = 0.0f;
  std::u16string shown;
};
NameEntry g_nameEntry;
uint32_t g_profileSelectObject = 0;
std::mutex g_nameEntryKeysMutex;
std::vector<char32_t> g_nameEntryKeys;
std::atomic<bool> g_nameEntryActive{false};

// The letter grid, one entry per cell; the last row holds the actions.
std::vector<std::vector<std::u16string>> NameEntryGrid(bool lower_case) {
  const std::u16string upper = u"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const std::u16string lower = u"abcdefghijklmnopqrstuvwxyz";
  const std::u16string& letters = lower_case ? lower : upper;
  std::vector<std::vector<std::u16string>> grid(4);
  for (size_t i = 0; i < letters.size(); ++i) grid[i < 13 ? 0 : 1].push_back(std::u16string(1, letters[i]));
  for (char16_t c : std::u16string(u"0123456789-.!")) grid[2].push_back(std::u16string(1, c));
  grid[3] = {u"Space", lower_case ? u"ABC" : u"abc", u"Delete", u"Done"};
  return grid;
}

// Sets up the profile screen's popup window the way createErrorMessage does, but larger, and shows it.
bool OpenNameEntry(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t title, uint32_t initial_text) {
  const uint32_t record = object ? REX_LOAD_U32(object + 512) : 0;
  const uint32_t window = record ? REX_LOAD_U32(record + 16) : 0;
  NameEntry& entry = g_nameEntry;
  if (!window || entry.active) return false;
  entry = NameEntry{};
  entry.active = true;
  entry.owner = object;
  entry.window = window;
  entry.title = title;
  for (uint32_t i = 0; initial_text && i < kNameEntryMaxLength; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(initial_text + i * 2));
    if (!c) break;
    entry.text.push_back(c);
  }
  CallGuest(ctx, base, sub_8210A470, window);
  CallGuest(ctx, base, sub_8210AEA8, window, 1, 2);
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  REX_STORE_U16(text_object + kTextLeft, 130);
  REX_STORE_U16(text_object + kTextRight, 510);
  REX_STORE_U16(text_object + kTextTop, 105);
  REX_STORE_U16(text_object + kTextBottom, 375);
  REX_STORE_U16(text_object + kTextMargin, 5);
  REX_STORE_U32(text_object + kTextColourCallbackData, 0);
  REX_STORE_U32(text_object + kTextColourCallback, kTextDefaultMenuColourCallback);
  REX_STORE_U32(text_object + 148, 1);
  REX_STORE_U32(text_object + 152, 1);
  CallGuest(ctx, base, sub_822CC508, kProfilePopupHiddenEvent, 0, 1);
  g_nameEntryActive.store(true);
  // Letters typed into the name would otherwise also press the controller buttons they are bound to.
  KameoSuppressKeybinds(true);
  return true;
}

// The save under the save list's cursor, found the way the delete path finds it: the page's list at record+60, offset by the two items above the saves.
uint32_t HoveredSave(uint8_t* base, uint32_t record) {
  const uint32_t window = REX_LOAD_U32(record + 8);
  if (!window) return 0;
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  const uint32_t selected = REX_LOAD_U8(REX_LOAD_U32(text_object + kTextMenu) + kMenuSelectedIndex);
  if (selected == 255 || selected < 2) return 0;
  uint32_t index = 0;
  uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem);
  for (; item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemSelectable) != 1) continue;
    if (index++ == selected) break;
  }
  if (!item || REX_LOAD_U32(item + kItemId) != kSaveSlotItemId) return 0;
  uint32_t save = REX_LOAD_U32(record + 60);
  for (uint32_t i = 0; save && i < selected - 2; ++i) save = REX_LOAD_U32(save + 4);
  return save;
}

void FinishNameEntry(PPCContext& ctx, uint8_t* base, bool accepted) {
  NameEntry& entry = g_nameEntry;
  if (entry.result_buffer && entry.result_capacity) {
    const std::u16string& text = accepted ? entry.text : std::u16string();
    const uint32_t count = std::min<uint32_t>(uint32_t(text.size()), entry.result_capacity - 1);
    for (uint32_t i = 0; i < count; ++i) REX_STORE_U16(entry.result_buffer + i * 2, uint16_t(text[i]));
    REX_STORE_U16(entry.result_buffer + count * 2, 0);
  }
  // XOVERLAPPED.InternalLow is the result the profile screen polls for.
  if (entry.overlapped) REX_STORE_U32(entry.overlapped, accepted ? 0 : kErrorCancelled);
  if (entry.renaming) {
    const bool valid = accepted && entry.text.find_first_not_of(u' ') != std::u16string::npos;
    const bool renamed = valid && RenameSaveGame(entry.file_name, entry.text);
    const uint32_t record = REX_LOAD_U32(entry.owner + 512);
    REX_STORE_U32(record + 24, renamed ? kProfileStateReload : kProfileStateList);
    REX_STORE_U32(record + 28, 1);
  }
  if (entry.window) CallGuest(ctx, base, sub_8210A470, entry.window);
  CallGuest(ctx, base, sub_822CC508, kProfilePopupHiddenEvent, 1, 1);
  entry.active = false;
  g_nameEntryActive.store(false);
  KameoSuppressKeybinds(false);
  std::lock_guard<std::mutex> lock(g_nameEntryKeysMutex);
  g_nameEntryKeys.clear();
}

void TypeIntoName(char16_t c) {
  if (g_nameEntry.text.size() < kNameEntryMaxLength) g_nameEntry.text.push_back(c);
}

void EraseFromName() {
  if (!g_nameEntry.text.empty()) g_nameEntry.text.pop_back();
}

// Presses the grid cell under the cursor; returns true when it finishes the entry.
bool PressNameEntryCell() {
  NameEntry& entry = g_nameEntry;
  const auto grid = NameEntryGrid(entry.lower_case);
  const std::u16string& cell = grid[size_t(entry.row)][size_t(entry.column)];
  if (cell == u"Space") TypeIntoName(u' ');
  else if (cell == u"abc" || cell == u"ABC") entry.lower_case = !entry.lower_case;
  else if (cell == u"Delete") EraseFromName();
  else if (cell == u"Done") return true;
  else TypeIntoName(cell[0]);
  return false;
}

// Adds the entry's lines at one glyph size and reports whether every line stayed on one row and the whole text fits the box.
bool LayOutNameEntry(PPCContext& ctx, uint8_t* base, uint32_t window, const std::vector<std::pair<int, std::u16string>>& lines, size_t single_row_line, int size) {
  CallGuest(ctx, base, sub_8210A470, window);
  for (const auto& [offset, line] : lines) {
    if (const uint32_t text = WithLeadingTags(base, g_nameEntry.title, SizeTag(size + offset) + (line.empty() ? u" " : line))) AddTextItem(ctx, base, window, 0, 0, text, 0, false);
  }
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  const float inner = float(REX_LOAD_U16(text_object + kTextBottom) - REX_LOAD_U16(text_object + kTextTop) - 2 * REX_LOAD_U16(text_object + kTextMargin));
  float bottom = 0.0f;
  std::vector<float> heights;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    heights.push_back(LoadFloat(base, item + kItemHeight));
    bottom = std::max(bottom, LoadFloat(base, item + kItemTop) + heights.back());
  }
  if (heights.size() != lines.size()) return false;
  // A short line that cannot wrap gives the height of one row at the grid's size.
  const float row = heights[single_row_line] / float(size);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (heights[i] > row * float(size + lines[i].first) * 1.5f) return false;
  }
  return bottom <= inner;
}

void RefreshNameEntryWindow(PPCContext& ctx, uint8_t* base, uint32_t window) {
  NameEntry& entry = g_nameEntry;
  const auto grid = NameEntryGrid(entry.lower_case);
  // Each line carries a size offset from the grid's glyph size.
  std::vector<std::pair<int, std::u16string>> lines;
  lines.push_back({0, PlainGuestText(base, entry.title)});
  lines.push_back({-6, u""});
  lines.push_back({8, entry.text + (entry.cursor_on ? u"_" : u" ")});
  lines.push_back({-6, u""});
  for (size_t r = 0; r < grid.size(); ++r) {
    std::u16string line;
    for (size_t c = 0; c < grid[r].size(); ++c) {
      const bool selected = int32_t(r) == entry.row && int32_t(c) == entry.column;
      if (!line.empty()) line += r == grid.size() - 1 ? u"    " : u" ";
      line += selected ? u"[" + grid[r][c] + u"]" : u" " + grid[r][c] + u" ";
    }
    lines.push_back({0, line});
  }
  const size_t actions_line = lines.size() - 1;
  lines.push_back({-6, u""});
  // Button tags rather than glyph characters, so keyboard and mouse players see their keys.
  lines.push_back({-2, KameoKeyboardMouseMode() ? u"Type a name   Bksp Delete   Enter Done   Esc Cancel" : u"{button='A'} Type   {button='B'} Delete   {button='Y'} Case   {button='start'} Done"});
  std::u16string layout;
  for (const auto& [offset, line] : lines) layout += line + u"\n";
  if (layout == entry.shown) return;
  entry.shown = layout;
  // The largest glyph size at which nothing wraps and everything fits.
  for (int size = 20; size >= 10; size -= 2) {
    if (LayOutNameEntry(ctx, base, window, lines, actions_line, size) || size == 10) break;
  }
}

}  // namespace

bool KameoNameEntryActive() { return g_nameEntryActive.load(); }

void KameoNameEntryPushKey(char32_t key) {
  std::lock_guard<std::mutex> lock(g_nameEntryKeysMutex);
  if (g_nameEntryKeys.size() < 64) g_nameEntryKeys.push_back(key);
}

// j_XamShowKeyboardUI(user, flags, default, title, description, result, capacity, overlapped) -- answered by the in-game name entry, left pending until it finishes.
extern "C" REX_FUNC(sub_8251ED98) {
  if (!ctx.r10.u32 || !OpenNameEntry(ctx, base, g_profileSelectObject, ctx.r6.u32, ctx.r5.u32)) {
    __imp__sub_8251ED98(ctx, base);
    return;
  }
  NameEntry& entry = g_nameEntry;
  entry.result_buffer = ctx.r8.u32;
  entry.result_capacity = ctx.r9.u32;
  entry.overlapped = ctx.r10.u32;
  REX_STORE_U32(entry.overlapped, kErrorIoPending);
  ctx.r3.u64 = kErrorIoPending;
}

// handleSaveGameContextSensitiveText(obj) -- its timer at record+72 alternates the hint with "X Select Storage Device" past 1.5 s, so it is held at zero.
extern "C" REX_FUNC(sub_823ED2A0) {
  if (const uint32_t record = REX_LOAD_U32(ctx.r3.u32 + 512)) StoreFloat(base, record + 72, 0.0f);
  __imp__sub_823ED2A0(ctx, base);
}

// xfix_storybookProfileSelectObj_tick(obj) -- remembers the profile screen for the keyboard call, and drives the name entry while it is open.
extern "C" REX_FUNC(sub_823EC530) {
  const uint32_t object = ctx.r3.u32;
  g_profileSelectObject = object;
  NameEntry& entry = g_nameEntry;
  const uint32_t record = REX_LOAD_U32(object + 512);
  // X on an existing save renames it instead of opening the storage device selector.
  if (!entry.active && record && REX_LOAD_U32(record + 24) == kProfileStateList && REX_LOAD_U32(record + 28) == 0) {
    const int8_t player = int8_t(REX_LOAD_U8(REX_LOAD_U32(object + 8) + 146));
    const uint32_t pressed_address = PlayerStick(base, player) + kStickPressed;
    const uint32_t pressed = REX_LOAD_U32(pressed_address);
    const uint32_t save = (pressed & REX_LOAD_U32(kCommitTestButtonMask) & kButtonX) != 0 ? HoveredSave(base, record) : 0;
    if (save && OpenNameEntry(ctx, base, object, GuestWideString(u"Rename Saved Game"), REX_LOAD_U32(save + 8))) {
      REX_STORE_U32(pressed_address, pressed & ~kButtonX);
      entry.renaming = true;
      for (uint32_t name = REX_LOAD_U32(save + 24), i = 0; name && i < 64; ++i) {
        const char c = char(REX_LOAD_U8(name + i));
        if (!c) break;
        entry.file_name.push_back(c);
      }
      REX_STORE_U32(record + 24, kProfileStateRenaming);
    }
  }
  __imp__sub_823EC530(ctx, base);
  if (!entry.active || entry.owner != object) return;
  const uint32_t window = entry.window;
  const float frame_time = LoadFloat(base, kFrameTime);
  entry.blink += frame_time;
  if (entry.blink >= 0.5f) {
    entry.blink = 0.0f;
    entry.cursor_on = !entry.cursor_on;
  }
  bool finish = false;
  bool cancel = false;
  {
    std::vector<char32_t> keys;
    {
      std::lock_guard<std::mutex> lock(g_nameEntryKeysMutex);
      keys.swap(g_nameEntryKeys);
    }
    for (char32_t key : keys) {
      if (key == 8) EraseFromName();
      else if (key == 13) finish = true;
      else if (key == 27) cancel = true;
      else if (key >= 32 && key < 0xFFFF && key != 127) TypeIntoName(char16_t(key));
    }
  }
  const uint32_t stick = ControllerStick(base, REX_LOAD_U8(kPlayerControllers));
  const uint32_t mask = REX_LOAD_U32(kCommitTestButtonMask);
  uint32_t pressed = REX_LOAD_U32(stick + kStickPressed) & mask;
  uint32_t held = REX_LOAD_U32(stick + kStickHeld) & mask;
  // The A press that opened the entry is still down, so the controller is ignored until every button is released.
  if (!entry.controller_ready) {
    entry.controller_ready = (held & 0xFFFF) == 0;
    pressed = 0;
    held = 0;
  }
  if (pressed & kButtonA) finish = PressNameEntryCell() || finish;
  if (pressed & kButtonB) EraseFromName();
  if (pressed & kButtonY) entry.lower_case = !entry.lower_case;
  if (pressed & 0x0010) finish = true;
  if (pressed & 0x0020) cancel = true;
  // D-pad or stick moves the grid cursor, repeating while held.
  const float x = LoadFloat(base, stick + kStickLeftX);
  const float y = LoadFloat(base, stick + kStickLeftY);
  int32_t direction = 0;
  if ((held & 0x1) || y > 0.5f) direction = 1;
  else if ((held & 0x2) || y < -0.5f) direction = 2;
  else if ((held & 0x4) || x < -0.5f) direction = 3;
  else if ((held & 0x8) || x > 0.5f) direction = 4;
  bool move = false;
  if (direction == 0) {
    entry.held_direction = 0;
  } else if (direction != entry.held_direction) {
    entry.held_direction = direction;
    entry.repeat = 0.4f;
    move = true;
  } else if ((entry.repeat -= frame_time) <= 0.0f) {
    entry.repeat = 0.12f;
    move = true;
  }
  if (move) {
    const auto grid = NameEntryGrid(entry.lower_case);
    const int32_t rows = int32_t(grid.size());
    if (direction == 1) entry.row = (entry.row + rows - 1) % rows;
    if (direction == 2) entry.row = (entry.row + 1) % rows;
    const int32_t columns = int32_t(grid[size_t(entry.row)].size());
    entry.column = std::min(entry.column, columns - 1);
    if (direction == 3) entry.column = (entry.column + columns - 1) % columns;
    if (direction == 4) entry.column = (entry.column + 1) % columns;
  }
  if (finish || cancel) {
    FinishNameEntry(ctx, base, finish && !cancel);
    return;
  }
  RefreshNameEntryWindow(ctx, base, window);
}
