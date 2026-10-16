// Button prompts with keyboard and mouse: {button} glyphs printed as the key that does it, and prompt bars widened to fit the names.

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "input/input.h"
#include "input/input_internal.h"
#include "ui/hud/key_labels.h"
#include "ui/text_window.h"

using namespace kameo::ui;

namespace {

// The player whose viewport iconRender is drawing, or -1 outside it.
thread_local int t_hudViewPlayer = -1;

// The key a {button='...'} tag stands for with keyboard and mouse: in a menu A and B are the mouse buttons and the triggers the wheel, in play every button is its binding. printButton reads the tag's first letter, and the trigger tags are localised words for left and right.
std::u16string KeyboardButtonLabel(uint8_t* base, uint32_t tag) {
  std::u16string name;
  for (uint32_t i = 0; tag && i < 16; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(tag + i * 2));
    if (!c) break;
    name.push_back(c);
  }
  if (name.empty()) return {};
  const bool menu = !kameo::input::detail::g_mouseLook;
  switch (name[0]) {
    case u'A': return menu ? u"LMB" : KeyLabel(KameoKeybindForSave("keybind_a"));
    case u'B': return menu ? u"RMB" : KeyLabel(KameoKeybindForSave("keybind_b"));
    case u'X': return KeyLabel(KameoKeybindForSave("keybind_x"));
    case u'Y': return KeyLabel(KameoKeybindForSave("keybind_y"));
    case u'D':
    case u'R':
      if (name == u"DPAD") return u"Arrows";
      return menu ? u"Wheel Down" : KeyLabel(KameoKeybindForSave("keybind_right_trigger"));
    case u'G':
    case u'I':
    case u'L':
    case u'S':
      return menu ? u"Wheel Up" : KeyLabel(KameoKeybindForSave("keybind_left_trigger"));
    case u'b':
      if (name == u"black") return KeyLabel(KameoKeybindForSave("keybind_right_shoulder"));
      if (name == u"back") return KeyLabel(KameoKeybindForSave("keybind_back"));
      return {};
    case u's': return KeyLabel(KameoKeybindForSave("keybind_start"));
    case u'w': return KeyLabel(KameoKeybindForSave("keybind_left_shoulder"));
    default: return {};
  }
}

}  // namespace

std::u16string kameo::ui::KeyLabel(const std::string& binding) {
  std::string key = binding.substr(0, binding.find(','));
  if (const size_t plus = key.rfind('+'); plus != std::string::npos && plus + 1 < key.size()) key = key.substr(plus + 1);
  static const std::pair<const char*, const char*> kFriendly[] = {
      {"Return", "Enter"}, {"Escape", "Esc"}, {"Backspace", "Bksp"}, {"LBracket", "["}, {"RBracket", "]"}, {"Quote", "'"}, {"Semicolon", ";"}, {"Backtick", "`"},
  };
  for (const auto& [name, label] : kFriendly) {
    if (key == name) key = label;
  }
  if (key.empty()) return u"-";
  return std::u16string(key.begin(), key.end());
}

// In split-screen each player's HUD names the controls that player uses; otherwise any keyboard player switches every prompt to keys.
bool kameo::ui::KeyboardLabelsFor(int player) {
  if (player >= 0 && player < 2 && KameoCoopInputMode() != 0) return KameoPlayerUsesKeyboard(player);
  return KameoKeyboardMouseMode();
}

// kameoFontControlTag::printButton(tag, fontState, value) -- with keyboard and mouse the {button} glyph is printed as the key that does it instead; measuring goes through the same fontPrintSubstring, so layouts size to the label.
extern "C" REX_FUNC(sub_8210B828) {
  if (!KeyboardLabelsFor(t_hudViewPlayer)) {
    __imp__sub_8210B828(ctx, base);
    return;
  }
  const std::u16string label = KeyboardButtonLabel(base, ctx.r5.u32);
  const uint32_t text = label.empty() ? 0 : GuestWideString(label);
  if (!text) {
    __imp__sub_8210B828(ctx, base);
    return;
  }
  PPCContext call = ctx;
  call.r3.u64 = ctx.r4.u32;
  call.r4.u64 = text;
  call.r5.u64 = 0;
  call.r6.u64 = 0x7FFFFFFE;
  call.r7.u64 = 0;
  sub_820FE390(call, base);
  ctx.r3.u64 = 1;
}

// iconRender(object, message, renderInfo, icons) -- runs once per viewport, and renderInfo+468 is that viewport's player, the one it filters icons by.
extern "C" REX_FUNC(sub_822CFEC0) {
  const int previous = t_hudViewPlayer;
  t_hudViewPlayer = ctx.r5.u32 ? int(REX_LOAD_U32(ctx.r5.u32 + 468)) : -1;
  __imp__sub_822CFEC0(ctx, base);
  t_hudViewPlayer = previous;
}

// Key names are words where the glyphs were single characters, so a prompt bar low on the screen (the storybook's "Select / Back" strip) is widened when a line with {button} tags goes into it; the box model is drawn from these edges and the text is clipped to them.
void KameoWidenPromptBar(uint8_t* base, uint32_t window, uint32_t text) {
  if (!window || !text || !KameoKeyboardMouseMode()) return;
  bool tagged = false;
  const char16_t kTag[] = u"{button=";
  for (uint32_t i = 0, matched = 0; i < 512 && !tagged; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(text + i * 2));
    if (!c) break;
    matched = c == kTag[matched] ? matched + 1 : (c == kTag[0] ? 1 : 0);
    tagged = matched == 8;
  }
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  if (!tagged || !text_object || int16_t(REX_LOAD_U16(text_object + kTextTop)) < 330) return;
  constexpr int16_t kExtra = 70;
  static std::mutex mutex;
  static std::unordered_map<uint32_t, std::pair<int16_t, int16_t>> widened;
  std::lock_guard<std::mutex> lock(mutex);
  // The window is an object header: a deleted window's memory is reused, but the serial objectCreateFull stamps at +76 never is.
  const uint32_t serial = REX_LOAD_U32(window + 76);
  const int16_t left = int16_t(REX_LOAD_U16(text_object + kTextLeft));
  const int16_t right = int16_t(REX_LOAD_U16(text_object + kTextRight));
  if (const auto found = widened.find(serial); found != widened.end() && found->second.first == left && found->second.second == right) return;
  const int16_t new_left = std::max<int16_t>(10, left - kExtra);
  const int16_t new_right = std::min<int16_t>(630, right + kExtra);
  REX_STORE_U16(text_object + kTextLeft, uint16_t(new_left));
  REX_STORE_U16(text_object + kTextRight, uint16_t(new_right));
  widened[serial] = {new_left, new_right};
}
