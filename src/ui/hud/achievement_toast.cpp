#include "ui/hud/achievement_toast.h"

#include <cmath>
#include <deque>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "input/guest_controls.h"
#include "ui/hud/fps_counter.h"
#include "ui/text_window.h"

REXCVAR_DEFINE_BOOL(kameo_toast_test, false, "Kameo", "Show a sample achievement notice once, to check the text box without earning one; clears itself.");

using namespace kameo::ui;
using kameo::input::kFrameTime;
using kameo::input::kPlayerList;

namespace {

// The same InGameText window the FPS counter uses, owned by player 1, so it only exists in gameplay.
constexpr uint32_t kInGameTextName = 0x82071280;
constexpr uint32_t kLevelNameTextWindowReady = 0x824452F8;
constexpr uint32_t kGenericDeleteDuringCreate = 0x82253270;

// A banner across the top centre of the 640x480 layout.
constexpr const char16_t* kToastStyle = u"{alignment='centre' size='16'}";
constexpr int16_t kToastLeft = 150;
constexpr int16_t kToastRight = 490;
constexpr int16_t kToastTop = 22;
constexpr int16_t kToastMargin = 4;
constexpr float kToastSeconds = 4.5f;

struct Notice {
  std::u16string label;
  uint32_t gamerscore = 0;
};

std::mutex g_queueMutex;
std::deque<Notice> g_queue;

struct Toast {
  uint32_t slot = 0;
  uint32_t owner = 0;
  bool pending = false;
  uint32_t pending_ticks = 0;
  bool showing = false;
  float remaining = 0.0f;
};
Toast g_toast;

std::u16string Widen(const std::string& text) {
  std::u16string wide;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    uint32_t point = c;
    size_t length = 1;
    if (c >= 0xF0) { point = c & 0x07; length = 4; }
    else if (c >= 0xE0) { point = c & 0x0F; length = 3; }
    else if (c >= 0xC0) { point = c & 0x1F; length = 2; }
    for (size_t k = 1; k < length && i + k < text.size(); ++k) point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    i += length;
    if (point >= 0x10000) {
      point -= 0x10000;
      wide.push_back(char16_t(0xD800 + (point >> 10)));
      wide.push_back(char16_t(0xDC00 + (point & 0x3FF)));
    } else {
      wide.push_back(char16_t(point));
    }
  }
  return wide;
}

bool WindowAlive(uint8_t* base, uint32_t window, uint32_t owner) {
  if (!window || REX_LOAD_U32(kPlayerList) != owner || !KameoGuestRangeReadable(window + kWindowTextObject, 4)) return false;
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  return text_object && KameoGuestRangeReadable(text_object, 260) && REX_LOAD_U32(text_object + kTextMenu) && REX_LOAD_U32(text_object + kTextWindow) == window;
}

}  // namespace

void KameoPushAchievementToast(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(g_queueMutex);
  g_queue.push_back({Widen(event.achievement.label), event.achievement.gamerscore});
}

bool KameoAchievementToastPending() { return g_toast.pending; }

void KameoTickAchievementToast(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(kameo_toast_test)) {
    REXCVAR_SET(kameo_toast_test, false);
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back({u"Sample Achievement", 20});
  }
  Toast& toast = g_toast;
  if (!toast.slot) {
    toast.slot = rex::system::kernel_state()->memory()->SystemHeapAlloc(4);
    if (!toast.slot) return;
    REX_STORE_U32(toast.slot, 0);
  }
  uint32_t window = REX_LOAD_U32(toast.slot);
  if (window && !WindowAlive(base, window, toast.owner)) {
    // Torn down by a level change before its delete list cleared the slot; forgotten, never touched.
    REX_STORE_U32(toast.slot, 0);
    window = 0;
    toast.pending = false;
    toast.showing = false;
  }
  if (toast.showing) {
    toast.remaining -= LoadFloat(base, kFrameTime);
    if (toast.remaining > 0.0f) return;
    toast.showing = false;
    if (window) {
      REX_STORE_U32(toast.slot, 0);
      CallGuest(ctx, base, sub_82250C58, window);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty()) return;
  }
  // Taken only once the box can show it, so a notice earned on the menu waits for gameplay rather than vanishing.
  const uint32_t player = REX_LOAD_U32(kPlayerList);
  if (!player) return;
  if (!window && (toast.pending || KameoFpsCounterPending())) {
    // One creation in flight at a time: the ready callback cannot tell two windows for one owner apart.
    if (toast.pending && ++toast.pending_ticks > 600) toast.pending = false;
    return;
  }
  if (!window) {
    toast.pending = true;
    toast.pending_ticks = 0;
    toast.owner = player;
    CallGuest(ctx, base, sub_82256CF8, kInGameTextName, kLevelNameTextWindowReady, player, kGenericDeleteDuringCreate);
    return;
  }
  Notice notice;
  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    notice = g_queue.front();
    g_queue.pop_front();
  }
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  CallGuest(ctx, base, sub_8210A470, window);
  const std::u16string text = std::u16string(kToastStyle) + u"Achievement Unlocked\n" + notice.label + u"   " + Number(int32_t(notice.gamerscore), u"G");
  const uint32_t guest_text = GuestWideString(text);
  if (!guest_text) return;
  AddTextItem(ctx, base, window, 0, 0, guest_text, 0, false);
  REX_STORE_U16(text_object + kTextBottom, uint16_t(kToastTop + int32_t(std::ceil(TextContentBottom(base, text_object))) + 2));
  toast.showing = true;
  toast.remaining = kToastSeconds;
}

bool KameoClaimAchievementToastWindow(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t owner) {
  Toast& toast = g_toast;
  if (!toast.pending || !owner || owner != toast.owner || !window) return false;
  toast.pending = false;
  CallGuest(ctx, base, sub_82253718, owner, window, toast.slot);
  CallGuest(ctx, base, sub_8210A470, window);
  CallGuest(ctx, base, sub_8210AEA8, window, 1, 3);
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  REX_STORE_U16(text_object + kTextLeft, uint16_t(kToastLeft));
  REX_STORE_U16(text_object + kTextRight, uint16_t(kToastRight));
  REX_STORE_U16(text_object + kTextTop, uint16_t(kToastTop));
  REX_STORE_U16(text_object + kTextBottom, uint16_t(kToastTop + 40));
  REX_STORE_U16(text_object + kTextMargin, uint16_t(kToastMargin));
  return true;
}
