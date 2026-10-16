// The mouse in the game's menus: hover selects, left click presses A, right click presses B and the wheel steps the cursor.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <rex/cvar.h>

#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "gpu/replay/book_pick.h"
#include "input/input.h"
#include "ui/menu_pointer/menu_ui_state.h"
#include "ui/text_window.h"

using namespace kameo::ui;

namespace {

constexpr uint32_t kMessageBoxOwners = 0x827FD4B8;
constexpr uint32_t kMessageBoxOwnersEnd = 0x827FD788;

// Where each selectable line of a text window is drawn, mirroring xfix_textWindow_render: lines stack under the previous one (or overwrite it), lines whose style asks for it are centred as a block, and a scrolling window shifts everything up by its scroll offset.
int32_t MenuItemAt(uint8_t* base, uint32_t text_object, float x, float y) {
  const float left = float(int16_t(REX_LOAD_U16(text_object + kTextLeft)));
  const float right = float(int16_t(REX_LOAD_U16(text_object + kTextRight)));
  const float top = float(int16_t(REX_LOAD_U16(text_object + kTextTop)));
  const float bottom = float(int16_t(REX_LOAD_U16(text_object + kTextBottom)));
  const float margin = float(REX_LOAD_U16(text_object + kTextMargin));
  if (x < left - margin || x > right + margin || y < top - margin || y > bottom + margin) return -1;
  const uint32_t menu = REX_LOAD_U32(text_object + kTextMenu);
  float centred = top + (bottom - top) * 0.5f;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemCentred) == 1 && REX_LOAD_U8(item + kItemOverwrite) == 0) centred -= LoadFloat(base, item + kItemHeight) * 0.5f;
  }
  float line = top - ((REX_LOAD_U16(menu) & kMenuFlagScrolling) != 0 ? LoadFloat(base, menu + kMenuScroll) : 0.0f);
  float previous_height = 0.0f;
  int32_t index = 0;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    const float height = LoadFloat(base, item + kItemHeight);
    const bool overwrite = REX_LOAD_U8(item + kItemOverwrite) != 0;
    if (!overwrite) line += previous_height;
    if (REX_LOAD_U32(item + kItemCentred) == 1 && !overwrite) {
      line = centred;
      centred += height;
    }
    if (REX_LOAD_U32(item + kItemSelectable) == 1) {
      if (y >= line && y < line + height) return index;
      ++index;
    }
    previous_height = height;
  }
  return -1;
}

// Hover selects, left click presses A on the line under the pointer, right click presses B and the wheel steps the cursor; returns true when a press ran, since its callback may have rebuilt or closed the window.
bool DriveMenuWithMouse(PPCContext& ctx, uint8_t* base, uint32_t text_object) {
  const uint32_t window = REX_LOAD_U32(text_object + kTextWindow);
  const uint32_t menu = REX_LOAD_U32(text_object + kTextMenu);
  if (!window || !menu || REX_LOAD_U8(text_object + 233) != 0 || (REX_LOAD_U16(menu) & 0x80) != 0) return false;
  int32_t selectables = 0;
  for (uint32_t item = REX_LOAD_U32(text_object + kTextFirstItem); item; item = REX_LOAD_U32(item + kItemNext)) {
    if (REX_LOAD_U32(item + kItemSelectable) == 1) ++selectables;
  }
  if (selectables == 0) return false;
  if (REX_LOAD_U8(text_object + 252) == 0) {
    for (uint32_t owner = kMessageBoxOwners; owner < kMessageBoxOwnersEnd; owner += 36) {
      if (REX_LOAD_U32(owner) != 0) return false;
    }
  }
  PPCContext check = ctx;
  check.r3.u64 = text_object;
  sub_82107A08(check, base);
  if (check.r3.u32 == 0) return false;
  KameoNoteMenuOpen(text_object);
  if (!KameoKeyCaptureTarget().empty() || !KameoPadCaptureTarget().empty() || (KameoKeyboardMouseMode() && KameoBookPointerActive())) return false;
  const bool pause_menu = IsLivePauseMenuWindow(window);
  const bool book_page = !pause_menu && RenderedOntoBookPage(text_object);
  if (!pause_menu && !book_page) return false;
  const KameoMouseFrame& mouse = KameoMouseThisTick();
  float pointer_x = 0.0f;
  float pointer_y = 0.0f;
  uint32_t moves = 0;
  if (!KameoMousePointerNow(&pointer_x, &pointer_y, &moves)) return false;
  // Movement is judged per window against the live pointer, so a window updated on a tick the input snapshot missed still follows it.
  // The GPU pick answers a few frames after the pointer moves, so hover keeps following it for a short while after the last movement.
  struct PointerSeen {
    uint32_t moves = 0;
    uint32_t tick = 0;
  };
  static std::unordered_map<uint32_t, PointerSeen> last_moves;
  PointerSeen& seen = last_moves[text_object];
  if (seen.moves != moves) {
    seen.moves = moves;
    seen.tick = UiTick();
  }
  const bool moved = UiTick() - seen.tick <= 8;
  float x = pointer_x;
  float y = pointer_y;
  // Widescreen stretches the frame, and the HUD layout draws menu text back at its own shape about the centre.
  if (pause_menu && REXCVAR_GET(kameo_gfx_widescreen) && REXCVAR_GET(kameo_hud_layout) != 0) {
    if (auto* gfx = kameo::gpu::GraphicsSystem::instance()) x = 320.0f + (pointer_x - 320.0f) * float(std::max(gfx->DisplayAspect() / (16.0 / 9.0), 1.0));
  }
  if (book_page) {
    float window_x = 0.0f;
    float window_y = 0.0f;
    if (KameoMouseWindowPoint(&window_x, &window_y)) kameo::gpu::RequestBookPick(window_x, window_y);
    // The page coordinate the GPU found under the pointer, exact however the pages flutter; the captured mesh is the fallback until it answers.
    bool hit = false;
    if (kameo::gpu::LatestBookPick(&x, &y, &hit)) {
      if (!hit) return false;
    } else if (!BookPagePoint(base, pointer_x, pointer_y, &x, &y)) {
      return false;
    }
    // Page windows draw their text at their style's scale from the window's inner top-left corner.
    const float kBookTextScale = WindowTextScale(base, text_object);
    const float margin = float(REX_LOAD_U16(text_object + kTextMargin));
    const float origin_x = float(int16_t(REX_LOAD_U16(text_object + kTextLeft))) - margin;
    const float origin_y = float(int16_t(REX_LOAD_U16(text_object + kTextTop))) - margin;
    x = origin_x + (x - origin_x) / kBookTextScale;
    y = origin_y + (y - origin_y) / kBookTextScale;
  }
  // Where the game really drew the lines wins; the layout rebuilt from the window's fields is only the fallback before its first render.
  int32_t hovered = DrawnLineAt(text_object, y);
  if (hovered == -2) {
    hovered = MenuItemAt(base, text_object, x, y);
  } else {
    const float margin = float(REX_LOAD_U16(text_object + kTextMargin));
    if (x < float(int16_t(REX_LOAD_U16(text_object + kTextLeft))) - margin || x > float(int16_t(REX_LOAD_U16(text_object + kTextRight))) + margin) hovered = -1;
  }
  if (moved && hovered >= 0) SelectMenuItem(ctx, base, text_object, hovered, false);
  // Several page windows take input in one tick, so the wheel and left click belong to the one under the pointer.
  // With keyboard and mouse the wheel steps the triggers instead, which the bonus menu pages with.
  if (mouse.wheel != 0 && hovered >= 0 && !KameoKeyboardMouseMode()) {
    const int32_t selected = REX_LOAD_U8(menu + kMenuSelectedIndex);
    KameoConsumeMouseWheel();
    SelectMenuItem(ctx, base, text_object, std::clamp(selected + (mouse.wheel > 0 ? -1 : 1), 0, selectables - 1), true);
  }
  // In Keyboard & Mouse mode the buttons reach the game as A and B presses, so a click only makes sure the line under the pointer is the one A lands on.
  if (KameoKeyboardMouseMode()) {
    if (mouse.left_clicks > 0 && hovered >= 0) {
      KameoConsumeMouseClicks();
      SelectMenuItem(ctx, base, text_object, hovered, false);
    }
    return false;
  }
  const bool left = mouse.left_clicks > 0 && hovered >= 0;
  const bool right = mouse.right_clicks > 0 && REX_LOAD_U32(menu + kMenuBackCallback) != 0;
  if (left || right) KameoConsumeMouseClicks();
  if (left) {
    SelectMenuItem(ctx, base, text_object, hovered, false);
    const uint32_t item = SelectableItem(base, text_object, hovered);
    const uint32_t callback = item ? REX_LOAD_U32(item + kItemCallback) : 0;
    if (!callback) return false;
    PlayMenuSound(ctx, base, REX_LOAD_U32(menu + kMenuSelectSound));
    CallMenuCallback(ctx, base, callback, window, uint32_t(hovered), REX_LOAD_U32(menu + kMenuCallbackData));
    return true;
  }
  if (right) {
    const uint32_t callback = REX_LOAD_U32(menu + kMenuBackCallback);
    if (!callback) return false;
    PlayMenuSound(ctx, base, REX_LOAD_U32(menu + kMenuBackSound));
    CallMenuCallback(ctx, base, callback, window, 0, REX_LOAD_U32(menu + kMenuCallbackData));
    return true;
  }
  return false;
}

}  // namespace

// textWindow_tick(textObject) -- menus take the mouse pointer before the game reads the controller, and any selectable window marks a menu as open so the pointer is freed from mouse look.
extern "C" REX_FUNC(sub_82107D88) {
  if (ctx.r3.u32 && DriveMenuWithMouse(ctx, base, ctx.r3.u32)) return;
  __imp__sub_82107D88(ctx, base);
}

// ReplayObjTick(replay) -- the storybook front end's object; while its state at +256 is anything but inactive (75150789, ReplayObjInactive) the pointer is freed as for an open menu.
extern "C" REX_FUNC(sub_823E7C08) {
  if (ctx.r3.u32 && REX_LOAD_U32(ctx.r3.u32 + 256) != 75150789) KameoNoteMenuOpen(0);
  __imp__sub_823E7C08(ctx, base);
}
