// The moveset HUD with keyboard and mouse: each trigger move's name gains the key that does it, in place of the trigger icon.

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>

#include "input/input.h"
#include "ui/hud/key_labels.h"
#include "ui/text_window.h"

using namespace kameo::ui;

namespace {

enum class IconLabelPlace { Above, Below };

struct IconLabelRect {
  int16_t left = 0;
  int16_t right = 0;
  int16_t top = 0;
  int16_t bottom = 0;
  bool operator==(const IconLabelRect& other) const { return left == other.left && right == other.right && top == other.top && bottom == other.bottom; }
};

struct IconLabelSlot {
  IconLabelRect original;
  IconLabelRect adjusted;
};

constexpr int16_t kIconLabelLineHeight = 24;
std::mutex g_iconLabelMutex;
std::unordered_map<uint32_t, IconLabelSlot> g_iconLabelSlots;
std::unordered_map<uint32_t, std::u16string> g_iconLabelBuilt;
// xfix_buildLocalisedMessageString wraps and places a line by the window's rect when it is added, so each label's rect at that moment is kept to rebuild it once the rect has grown.
std::unordered_map<uint32_t, IconLabelRect> g_iconLabelBuiltRect;
thread_local uint32_t t_iconLRRecord = 0;
thread_local bool t_iconLRKeyboard = false;
thread_local std::array<uint32_t, 3> t_iconLRTextures{};

std::u16string IconLabelKey(int32_t slot) {
  const std::u16string left = KeyLabel(KameoKeybindForSave("keybind_left_trigger"));
  const std::u16string right = KeyLabel(KameoKeybindForSave("keybind_right_trigger"));
  return slot == 0 ? left : slot == 1 ? right : left + u"+" + right;
}

IconLabelRect LoadIconLabelRect(uint8_t* base, uint32_t text_object) {
  return {int16_t(REX_LOAD_U16(text_object + kTextLeft)), int16_t(REX_LOAD_U16(text_object + kTextRight)), int16_t(REX_LOAD_U16(text_object + kTextTop)), int16_t(REX_LOAD_U16(text_object + kTextBottom))};
}

void IconBoxAdd(uint8_t* base, uint32_t texture, uint32_t centre, uint32_t scale, float& left, float& right, float& top, float& bottom) {
  if (!texture) return;
  const float x = LoadFloat(base, centre);
  const float y = LoadFloat(base, centre + 4);
  const float s = LoadFloat(base, scale);
  const float half_width = float(REX_LOAD_U16(texture + 36)) * s * 0.5f;
  const float half_height = float(REX_LOAD_U16(texture + 38)) * s * 0.5f;
  left = std::min(left, x - half_width);
  right = std::max(right, x + half_width);
  top = std::min(top, y - half_height);
  bottom = std::max(bottom, y + half_height);
}

bool IconBox(uint8_t* base, uint32_t record, int32_t slot, float& left, float& right, float& top, float& bottom) {
  left = top = 1e9f;
  right = bottom = -1e9f;
  if (slot == 0) IconBoxAdd(base, REX_LOAD_U32(record), record + 12, record + 28, left, right, top, bottom);
  if (slot == 1) IconBoxAdd(base, REX_LOAD_U32(record + 4), record + 60, record + 72, left, right, top, bottom);
  if (slot == 2) {
    IconBoxAdd(base, REX_LOAD_U32(record), record + 92, record + 108, left, right, top, bottom);
    IconBoxAdd(base, REX_LOAD_U32(record + 4), record + 100, record + 112, left, right, top, bottom);
  }
  return left <= right && top <= bottom;
}

// The rect the game gave the label, looking through an adjustment made below.
IconLabelRect OriginalIconLabelRect(uint8_t* base, uint32_t text_object) {
  const IconLabelRect current = LoadIconLabelRect(base, text_object);
  const auto found = g_iconLabelSlots.find(text_object);
  return found != g_iconLabelSlots.end() && found->second.adjusted == current ? found->second.original : current;
}

// The key goes on its own line, below the name only when the icon sat below it: on one line with the name, a long name wrapped out of the one-line box.
IconLabelPlace PlaceIconLabel(uint8_t* base, uint32_t record, int32_t slot, const IconLabelRect& rect) {
  float left, right, top, bottom;
  if (!IconBox(base, record, slot, left, right, top, bottom)) return IconLabelPlace::Above;
  return (top + bottom) * 0.5f > rect.bottom ? IconLabelPlace::Below : IconLabelPlace::Above;
}

// The player an icon belongs to, the byte IconLRTick indexes iconPrefs by; -1 shows it to every player.
int IconOwner(uint8_t* base, uint32_t icon) {
  const uint32_t data = REX_LOAD_U32(icon + 44);
  return data ? int(int8_t(REX_LOAD_U8(data + 146))) : -1;
}

int32_t IconLabelSlotOf(uint8_t* base, uint32_t record, uint32_t window) {
  for (int32_t i = 0; i < 3; ++i) {
    if (REX_LOAD_U32(record + 36 + 40 * uint32_t(i)) == window) return i;
  }
  return -1;
}

// Grows each label's rect by a line for the key above or below the move name.
void ArrangeIconLabels(uint8_t* base, uint32_t record, bool keyboard) {
  std::lock_guard<std::mutex> lock(g_iconLabelMutex);
  for (int32_t i = 0; i < 3; ++i) {
    const uint32_t window = REX_LOAD_U32(record + 36 + 40 * uint32_t(i));
    const uint32_t text_object = window ? REX_LOAD_U32(window + kWindowTextObject) : 0;
    if (!text_object) continue;
    const IconLabelRect original = OriginalIconLabelRect(base, text_object);
    IconLabelRect adjusted = original;
    if (keyboard) {
      // The game's rect is one line of its style tall, and xfix_textWindow_render leaves out a line that does not fit whole, so the key's line adds that height and a little more.
      const int16_t line = int16_t(std::max<int>(original.bottom - original.top, kIconLabelLineHeight) + 4);
      if (PlaceIconLabel(base, record, i, original) == IconLabelPlace::Above) adjusted.top = int16_t(original.top - line);
      else adjusted.bottom = int16_t(original.bottom + line);
      // A key name wider than the box would wrap too; it grows away from the edge its alignment (left, right, centre) anchors the text to.
      const int16_t grow = int16_t(IconLabelKey(i).size() * 12 + 16);
      if (i == 0) adjusted.right = int16_t(adjusted.right + grow);
      else if (i == 1) adjusted.left = std::max<int16_t>(0, int16_t(adjusted.left - grow));
      else {
        adjusted.left = std::max<int16_t>(0, int16_t(adjusted.left - grow / 2));
        adjusted.right = int16_t(adjusted.right + grow / 2);
      }
    }
    REX_STORE_U16(text_object + kTextLeft, uint16_t(adjusted.left));
    REX_STORE_U16(text_object + kTextRight, uint16_t(adjusted.right));
    REX_STORE_U16(text_object + kTextTop, uint16_t(adjusted.top));
    REX_STORE_U16(text_object + kTextBottom, uint16_t(adjusted.bottom));
    const auto previous = g_iconLabelSlots.find(text_object);
    const bool settled = previous != g_iconLabelSlots.end() && previous->second.adjusted == adjusted;
    if (keyboard) g_iconLabelSlots[text_object] = {original, adjusted};
    else g_iconLabelSlots.erase(text_object);
    // A label laid out in a rect of another size has its lines wrapped for that size; once the rect holds still, a stale action id makes IconLRCheckContext build it again next tick.
    const auto built = g_iconLabelBuiltRect.find(text_object);
    if (keyboard && settled && built != g_iconLabelBuiltRect.end() && (built->second.right - built->second.left != adjusted.right - adjusted.left || built->second.bottom - built->second.top != adjusted.bottom - adjusted.top)) {
      g_iconLabelBuiltRect.erase(built);
      const uint32_t action = record + 40 + 40 * uint32_t(i);
      if (REX_LOAD_U32(action) != 0xFFFFFFFF) REX_STORE_U32(action, 0xFFFFFFFE);
    }
  }
}

}  // namespace

// The move name IconLRCheckContext is building for a trigger slot, with the key that does the move put on the side the trigger icon was.
uint32_t KameoIconLabelText(uint8_t* base, uint32_t window, uint32_t text) {
  const uint32_t record = t_iconLRRecord;
  if (!record || !window || !text || !t_iconLRKeyboard) return 0;
  const int32_t slot = IconLabelSlotOf(base, record, window);
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  if (slot < 0 || !text_object) return 0;
  std::u16string name;
  for (uint32_t i = 0; i < 256; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(text + i * 2));
    if (!c) break;
    name.push_back(c);
  }
  const std::u16string key = IconLabelKey(slot);
  IconLabelPlace place;
  {
    std::lock_guard<std::mutex> lock(g_iconLabelMutex);
    place = PlaceIconLabel(base, record, slot, OriginalIconLabelRect(base, text_object));
    g_iconLabelBuiltRect[text_object] = LoadIconLabelRect(base, text_object);
  }
  return GuestWideString(place == IconLabelPlace::Above ? key + u"\n" + name : name + u"\n" + key);
}

// IconLRTick(icon) -- rebuilds the move names when the input mode or trigger bindings change, and lays the labels out over the icons after the game positions them.
extern "C" REX_FUNC(sub_82456080) {
  const uint32_t icon = ctx.r3.u32;
  const uint32_t record = icon ? REX_LOAD_U32(icon + 152) : 0;
  const bool keyboard = icon && KeyboardLabelsFor(IconOwner(base, icon));
  if (record) {
    const std::u16string signature = keyboard ? IconLabelKey(2) : std::u16string();
    std::lock_guard<std::mutex> lock(g_iconLabelMutex);
    if (std::u16string& built = g_iconLabelBuilt[record]; built != signature) {
      built = signature;
      // An action id that no longer matches makes IconLRCheckContext clear and rebuild that slot's text.
      for (uint32_t i = 0; i < 3; ++i) {
        if (REX_LOAD_U32(record + 40 + 40 * i) != 0xFFFFFFFF) REX_STORE_U32(record + 40 + 40 * i, 0xFFFFFFFE);
      }
    }
  }
  t_iconLRRecord = record;
  t_iconLRKeyboard = keyboard;
  __imp__sub_82456080(ctx, base);
  t_iconLRRecord = 0;
  t_iconLRKeyboard = false;
  if (record) ArrangeIconLabels(base, record, keyboard);
}

// IconLRRender(icon) -- with keyboard and mouse the trigger icon textures are left out, the labels naming the keys instead.
extern "C" REX_FUNC(sub_82456338) {
  const uint32_t record = ctx.r3.u32 ? REX_LOAD_U32(ctx.r3.u32 + 152) : 0;
  if (record && KeyboardLabelsFor(IconOwner(base, ctx.r3.u32))) t_iconLRTextures = {REX_LOAD_U32(record), REX_LOAD_U32(record + 4), REX_LOAD_U32(record + 8)};
  __imp__sub_82456338(ctx, base);
  t_iconLRTextures = {};
}

// iconRenderIcon2DTrueXYScale(texture, x, y, xscale, yscale, alpha, rotation, filter)
extern "C" REX_FUNC(sub_822D13C8) {
  const uint32_t texture = ctx.r3.u32;
  if (texture && std::find(t_iconLRTextures.begin(), t_iconLRTextures.end(), texture) != t_iconLRTextures.end()) return;
  __imp__sub_822D13C8(ctx, base);
}
