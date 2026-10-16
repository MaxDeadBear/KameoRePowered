// The Wotnot's wisp pointer on the warrior and upgrade pages, moved by the mouse.

#include <algorithm>
#include <atomic>
#include <mutex>

#include <rex/cvar.h>

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"

REXCVAR_DECLARE(double, mnk_sensitivity);

using namespace kameo::input;
using namespace kameo::input::detail;

namespace {

// When BookPointerObj_tick last ran with its pointer showing.
std::atomic<int64_t> g_bookPointerSeen{0};

}  // namespace

bool KameoBookPointerActive() { return KameoSteadyMilliseconds() - g_bookPointerSeen.load() < 150; }

// BookPointerObj_tick(pointer) -- the Wotnot's wisp; record+2064 is zero while it is showing.
extern "C" REX_FUNC(sub_823CA940) {
  const uint32_t pointer = ctx.r3.u32;
  const uint32_t record = pointer ? REX_LOAD_U32(pointer + 512) : 0;
  if (record && REX_LOAD_U32(record + 2064) == 0) g_bookPointerSeen.store(KameoSteadyMilliseconds());
  __imp__sub_823CA940(ctx, base);
}

// xfix_BookPointerObj_handler_823CACB8(pointer) -- moves the wisp by the stick at pointer+244/+248 and then snaps it to targets; mouse travel is added first so the snapping and the matrices built after see it.
extern "C" REX_FUNC(sub_823CACB8) {
  const uint32_t pointer = ctx.r3.u32;
  // 0x82884E08 is set when the pointer is driven by network messages instead of the local pad.
  if (pointer && KameoKeyboardMouseMode() && REX_LOAD_U32(0x82884E08) == 0) {
    float dx = 0.0f;
    float dy = 0.0f;
    uint32_t width = 0;
    {
      std::lock_guard<std::mutex> lock(g_mouse.mutex);
      dx = g_mouse.pointer_dx;
      dy = g_mouse.pointer_dy;
      width = g_mouse.width;
      g_mouse.pointer_dx = 0.0f;
      g_mouse.pointer_dy = 0.0f;
    }
    if ((dx != 0.0f || dy != 0.0f) && width > 0) {
      // Half the window's width carries the wisp from the middle of the book to a page edge.
      const float units_per_pixel = 5.1f / float(width) * float(REXCVAR_GET(mnk_sensitivity));
      const float x = std::clamp(LoadFloat(base, pointer + 244) + dx * units_per_pixel, -2.9f, 2.9f);
      const float y = std::clamp(LoadFloat(base, pointer + 248) - dy * units_per_pixel, -2.3f, 2.3f);
      StoreFloat(base, pointer + 244, x);
      StoreFloat(base, pointer + 248, y);
    }
  }
  __imp__sub_823CACB8(ctx, base);
}
