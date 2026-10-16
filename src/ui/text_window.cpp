#include "ui/text_window.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>

namespace kameo::ui {

namespace {

constexpr uint32_t kHStringTable = 0x827556B8;

}  // namespace

thread_local bool t_addingOwnTextItem = false;

uint32_t GuestWideString(const std::u16string& text) {
  static std::mutex mutex;
  static std::unordered_map<std::u16string, uint32_t> cache;
  std::lock_guard<std::mutex> lock(mutex);
  if (const auto found = cache.find(text); found != cache.end()) return found->second;
  auto* memory = rex::system::kernel_state()->memory();
  const uint32_t address = memory ? memory->SystemHeapAlloc(uint32_t(text.size() + 1) * 2) : 0;
  if (!address) return 0;
  uint8_t* base = GuestBase();
  for (size_t i = 0; i <= text.size(); ++i) REX_STORE_U16(address + uint32_t(i) * 2, i < text.size() ? uint16_t(text[i]) : 0);
  cache.emplace(text, address);
  return address;
}

std::u16string Number(int32_t value, const char16_t* suffix) {
  std::u16string text;
  for (char c : std::to_string(value)) text += char16_t(c);
  return text + suffix;
}

void CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*), uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6) {
  PPCContext call = ctx;
  call.r3.u64 = r3;
  call.r4.u64 = r4;
  call.r5.u64 = r5;
  call.r6.u64 = r6;
  function(call, base);
}

uint32_t LocalisedString(PPCContext& ctx, uint8_t* base, int32_t id) {
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kHStringTable);
  call.r4.u64 = uint32_t(id);
  sub_8251DCE8(call, base);
  return call.r3.u32;
}

uint32_t StyledLike(uint8_t* base, uint32_t game_text, const std::u16string& label) {
  if (!game_text) return GuestWideString(label);
  std::u16string source;
  std::vector<bool> tag;
  bool in_tag = false;
  for (uint32_t i = 0; i < 512; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(game_text + i * 2));
    if (!c) break;
    const bool opens = !in_tag && c == u'{';
    source += c;
    tag.push_back(in_tag || opens);
    in_tag = (in_tag || opens) && c != u'}';
  }
  size_t first = source.size();
  size_t last = 0;
  for (size_t i = 0; i < source.size(); ++i) {
    if (tag[i] || source[i] == u' ' || source[i] == u'\t') continue;
    first = std::min(first, i);
    last = i;
  }
  if (first == source.size()) return GuestWideString(source + label);
  std::u16string styled;
  for (size_t i = 0; i < source.size(); ++i) {
    if (i == first) styled += label;
    if (tag[i] || i < first || i > last) styled += source[i];
  }
  return GuestWideString(styled);
}

uint32_t WithLeadingTags(uint8_t* base, uint32_t game_text, const std::u16string& label) {
  std::u16string prefix;
  bool in_tag = false;
  for (uint32_t i = 0; game_text && i < 512; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(game_text + i * 2));
    if (!c || (!in_tag && c != u'{' && c != u' ' && c != u'\n')) break;
    if (in_tag || c == u'{') prefix += c;
    in_tag = (in_tag || c == u'{') && c != u'}';
  }
  return GuestWideString(prefix + label);
}

std::u16string PlainGuestText(uint8_t* base, uint32_t text) {
  std::u16string plain;
  bool in_tag = false;
  for (uint32_t i = 0; text && i < 512; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(text + i * 2));
    if (!c) break;
    if (c == u'{') in_tag = true;
    else if (c == u'}') in_tag = false;
    else if (!in_tag) plain += c;
  }
  return plain;
}

std::u16string SizeTag(int size) {
  const std::string digits = std::to_string(size);
  return u"{size='" + std::u16string(digits.begin(), digits.end()) + u"'}";
}

// xfix_buildLocalisedMessageString(window, id, format, text, callback, float, _, -1, selectable)
void AddTextItem(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t id, uint32_t format, uint32_t text, uint32_t callback, bool selectable) {
  PPCContext call = ctx;
  call.r3.u64 = window;
  call.r4.u64 = id;
  call.r5.u64 = format;
  call.r6.u64 = text;
  call.r7.u64 = callback;
  call.f1.f64 = 0.0;
  call.r8.u64 = 0;
  call.r9.s64 = -1;
  call.r10.u64 = selectable ? 1 : 0;
  t_addingOwnTextItem = true;
  sub_8210A4C8(call, base);
  t_addingOwnTextItem = false;
}

float TextContentBottom(uint8_t* base, uint32_t text_object) {
  float content = 0.0f;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) content = std::max(content, LoadFloat(base, item + kItemTop) + LoadFloat(base, item + kItemHeight));
  return content;
}

uint32_t SelectedItemId(uint8_t* base, uint32_t window, uint32_t index) {
  if (!window) return 0;
  uint32_t item = REX_LOAD_U32(TextObjectOf(base, window) + kTextFirstItem);
  for (uint32_t seen = 0; item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemSelectable) != 1) continue;
    if (seen++ == index) return REX_LOAD_U32(item + kItemId);
  }
  return 0;
}

uint32_t SelectedIndex(uint8_t* base, uint32_t window) { return REX_LOAD_U8(MenuOf(base, TextObjectOf(base, window)) + kMenuSelectedIndex); }

uint32_t SelectableItem(uint8_t* base, uint32_t text_object, int32_t index) {
  int32_t seen = 0;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemSelectable) != 1) continue;
    if (seen++ == index) return item;
  }
  return 0;
}

void KeepCursor(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t index) {
  REX_STORE_U8(MenuOf(base, TextObjectOf(base, window)) + kMenuSelectedIndex, uint8_t(index));
  PPCContext scroll = ctx;
  scroll.r3.u64 = window;
  scroll.r4.u64 = index;
  scroll.f1.f64 = 0.0;
  sub_8210ACA0(scroll, base);
}

// The page lays its lines out from the window's top, less any scroll.
float TextLineCentre(uint8_t* base, uint32_t text_object, uint32_t line_id) {
  const uint32_t menu = MenuOf(base, text_object);
  const float scroll = (REX_LOAD_U16(menu) & kMenuFlagScrolling) ? LoadFloat(base, menu + kMenuScroll) : 0.0f;
  const float top = float(int16_t(REX_LOAD_U16(text_object + kTextTop)));
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemId) == line_id) return top + LoadFloat(base, item + kItemTop) + LoadFloat(base, item + kItemHeight) * 0.5f - scroll;
  }
  return -1.0f;
}

void PlayMenuSound(PPCContext& ctx, uint8_t* base, uint32_t sound) {
  if (sound != 0xFFFFFFFF) CallGuest(ctx, base, sub_822EA728, sound);
}

// REX_CALL_INDIRECT_FUNC expands against the `ctx` in scope, so the prepared registers shadow it.
void CallMenuCallback(PPCContext& ctx, uint8_t* base, uint32_t callback, uint32_t window, uint32_t index, uint32_t data) {
  PPCContext call = ctx;
  call.r3.u64 = window;
  call.r4.u64 = index;
  call.r5.u64 = data;
  call.ctr.u64 = callback;
  {
    PPCContext& ctx = call;
    REX_CALL_INDIRECT_FUNC(callback);
  }
}

void SelectMenuItem(PPCContext& ctx, uint8_t* base, uint32_t text_object, int32_t index, bool scroll) {
  const uint32_t window = REX_LOAD_U32(text_object + kTextWindow);
  const uint32_t menu = MenuOf(base, text_object);
  const uint8_t selected = REX_LOAD_U8(menu + kMenuSelectedIndex);
  if (index == selected) return;
  REX_STORE_U16(menu + kMenuPreviousIndex, selected);
  REX_STORE_U8(menu + kMenuSelectedIndex, uint8_t(index));
  if (scroll && (REX_LOAD_U16(menu) & kMenuFlagScrolling) != 0) {
    PPCContext call = ctx;
    call.r3.u64 = window;
    call.r4.u64 = uint32_t(index);
    call.f1.f64 = 0.167;
    sub_8210ACA0(call, base);
  }
  if (const uint32_t changed = REX_LOAD_U32(menu + kMenuChangedCallback)) CallMenuCallback(ctx, base, changed, window, uint32_t(index), REX_LOAD_U32(menu + kMenuCallbackData));
  PlayMenuSound(ctx, base, REX_LOAD_U32(menu + kMenuMoveSound));
}

}  // namespace kameo::ui
