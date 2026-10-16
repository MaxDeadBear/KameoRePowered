// The in-game FPS counter: a real InGameText window in the top-right corner, so it draws in the game's font and text box.

#include "ui/hud/fps_counter.h"

#include <cmath>
#include <string>

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "gpu/graphics_system.h"
#include "input/guest_controls.h"
#include "ui/menu_pointer/menu_ui_state.h"
#include "ui/text_window.h"

REXCVAR_DEFINE_BOOL(kameo_fps_counter, false, "Kameo", "Show the frame rate in a Kameo text box in the top-right corner.");

using namespace kameo::ui;
using kameo::input::kFrameTime;
using kameo::input::kPlayerList;

namespace {

// The counter's window is owned by player 1 and recreated whenever a level change deletes it.
constexpr uint32_t kInGameTextName = 0x82071280;
constexpr uint32_t kLevelNameTextWindowReady = 0x824452F8;
constexpr uint32_t kGenericDeleteDuringCreate = 0x82253270;

// A small box hugging the top-right corner of the 640x480 layout; kameoFontControlTag ignores scale, so size sets the glyph height in layout units.
constexpr const char16_t* kFpsCounterStyle = u"{size='12'}";
constexpr int16_t kFpsCounterLeft = 590;
constexpr int16_t kFpsCounterRight = 626;
constexpr int16_t kFpsCounterTop = 16;
constexpr int16_t kFpsCounterMargin = 2;

struct FpsCounter {
  uint32_t slot = 0;
  uint32_t owner = 0;
  bool pending = false;
  uint32_t pending_ticks = 0;
  float refresh = 0.0f;
  std::u16string shown;
};
FpsCounter g_fpsCounter;

thread_local bool t_renderingFpsCounter = false;

// A freed window's memory is released or reused before its delete list clears the slot, so its fields are only read where guest pages are still readable.
bool GuestRangeReadable(uint32_t address, uint32_t bytes) {
  if (address < 0x10000 || address > 0xFFFFFFFFu - bytes) return false;
  rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + bytes - 1) != rex::memory::PageAccess::kNoAccess;
}

}  // namespace

bool kameo::ui::IsFpsCounterText(uint8_t* base, uint32_t text_object) {
  const uint32_t slot = g_fpsCounter.slot;
  const uint32_t window = slot ? REX_LOAD_U32(slot) : 0;
  return window && text_object && REX_LOAD_U32(text_object + kTextWindow) == window;
}

void kameo::ui::MarkFpsCounterRender(bool rendering) { t_renderingFpsCounter = rendering; }

void KameoTickFpsCounter(PPCContext& ctx, uint8_t* base) {
  TickMenuUiState();
  FpsCounter& counter = g_fpsCounter;
  if (!counter.slot) {
    counter.slot = rex::system::kernel_state()->memory()->SystemHeapAlloc(4);
    if (!counter.slot) return;
    REX_STORE_U32(counter.slot, 0);
  }
  uint32_t window = REX_LOAD_U32(counter.slot);
  // A level change or quitting to the menu tears the window down before its delete list clears the slot, and a new player object means the old owner is gone with it; a dead window is forgotten, never touched.
  const uint32_t text_object = window && REX_LOAD_U32(kPlayerList) == counter.owner && GuestRangeReadable(window + kWindowTextObject, 4) ? REX_LOAD_U32(window + kWindowTextObject) : 0;
  if (window && (!text_object || !GuestRangeReadable(text_object, 260) || !REX_LOAD_U32(text_object + kTextMenu) || REX_LOAD_U32(text_object + kTextWindow) != window)) {
    REX_STORE_U32(counter.slot, 0);
    window = 0;
    counter.pending = false;
  }
  if (!REXCVAR_GET(kameo_fps_counter)) {
    if (window) {
      REX_STORE_U32(counter.slot, 0);
      CallGuest(ctx, base, sub_82250C58, window);
    }
    counter.pending = false;
    counter.shown.clear();
    return;
  }
  if (!window) {
    counter.shown.clear();
    const uint32_t player = REX_LOAD_U32(kPlayerList);
    if (!player || (counter.pending && ++counter.pending_ticks < 600)) return;
    counter.pending = true;
    counter.pending_ticks = 0;
    counter.owner = player;
    // objectCreateSimpleWithDelete(name, ready, owner, deleteDuringCreate); the ready callback is levelNameTextWindowReady, which the hook below intercepts for this owner.
    CallGuest(ctx, base, sub_82256CF8, kInGameTextName, kLevelNameTextWindowReady, player, kGenericDeleteDuringCreate);
    return;
  }
  counter.refresh -= LoadFloat(base, kFrameTime);
  if (counter.refresh > 0.0f && !counter.shown.empty()) return;
  counter.refresh = 0.5f;
  auto* gfx = kameo::gpu::GraphicsSystem::instance();
  const int32_t fps = gfx ? int32_t(gfx->GetFrameStats().fps + 0.5) : 0;
  const std::u16string text = Number(fps, u" FPS");
  if (text == counter.shown) return;
  counter.shown = text;
  CallGuest(ctx, base, sub_8210A470, window);
  const uint32_t guest_text = GuestWideString(std::u16string(kFpsCounterStyle) + text);
  if (!guest_text) return;
  AddTextItem(ctx, base, window, 0, 0, guest_text, 0, false);
  // The window only draws lines that fit whole, so its height follows the measured line.
  REX_STORE_U16(text_object + kTextBottom, uint16_t(kFpsCounterTop + int32_t(std::ceil(TextContentBottom(base, text_object))) + 1));
}

bool KameoSkipTextBoxShadow() {
  // xfix_textWindow_render draws the box model first in flat black (constant colour 0 at 0x82B718A8, enabled at 0x82B718B8) as its drop shadow.
  if (!t_renderingFpsCounter) return false;
  const uint8_t* base = GuestBase();
  return base && REX_LOAD_U32(0x82B718B8) != 0 && REX_LOAD_U32(0x82B718A8) == 0;
}

// levelNameTextWindowReady(textWindow, owner) -- when it is the FPS counter's window arriving, it is set up as a small box in the top-right corner instead of a level name banner.
extern "C" REX_FUNC(sub_824452F8) {
  FpsCounter& counter = g_fpsCounter;
  const uint32_t window = ctx.r3.u32;
  const uint32_t owner = ctx.r4.u32;
  if (!counter.pending || !owner || owner != counter.owner || !window) {
    __imp__sub_824452F8(ctx, base);
    return;
  }
  counter.pending = false;
  CallGuest(ctx, base, sub_82253718, owner, window, counter.slot);
  CallGuest(ctx, base, sub_8210A470, window);
  CallGuest(ctx, base, sub_8210AEA8, window, 1, 3);
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  REX_STORE_U16(text_object + kTextLeft, uint16_t(kFpsCounterLeft));
  REX_STORE_U16(text_object + kTextRight, uint16_t(kFpsCounterRight));
  REX_STORE_U16(text_object + kTextTop, uint16_t(kFpsCounterTop));
  REX_STORE_U16(text_object + kTextBottom, uint16_t(kFpsCounterTop + 20));
  REX_STORE_U16(text_object + kTextMargin, uint16_t(kFpsCounterMargin));
  counter.refresh = 0.0f;
}
