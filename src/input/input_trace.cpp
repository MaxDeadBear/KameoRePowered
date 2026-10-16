// Diagnostic logging of mouse movement and player two's input route, kept apart from the input code it watches.

#include <atomic>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/window.h>

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input_internal.h"

REXCVAR_DEFINE_BOOL(kameo_input_trace_mouse, false, "Kameo", "Log the mouse movements the game receives while the mouse turns the camera: position, relative travel and whether SDL has the pointer locked.");
REXCVAR_DECLARE(bool, kameo_story_splitscreen);

using namespace kameo::input;

namespace {

std::atomic<int64_t> g_lastP2KeyboardTrace{0};
std::atomic<int64_t> g_lastP2ControlsTrace{0};

}  // namespace

namespace kameo::input::detail {

void TraceMouseMove(rex::ui::MouseEvent& e) {
  static uint32_t traced = 0;
  if (!REXCVAR_GET(kameo_input_trace_mouse) || !g_mouseLook || traced >= 1500) return;
  ++traced;
  SDL_Window* focus = SDL_GetMouseFocus();
  REXLOG_INFO("[kameo] mouse trace {}: at {},{} travel {:.1f},{:.1f} window {}x{} locked {} driver {}", traced, e.x(), e.y(), e.dx(), e.dy(), e.target() ? e.target()->GetActualPhysicalWidth() : 0, e.target() ? e.target()->GetActualPhysicalHeight() : 0, focus && SDL_GetWindowRelativeMouseMode(focus), SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
}

void TraceP2KeyboardPoll(uint8_t* base, bool have_aggregate, bool have_device_keyboard, const rex::input::X_INPUT_STATE& aggregate, const rex::input::X_INPUT_STATE& device_keyboard, const rex::input::X_INPUT_STATE& keyboard) {
  const int64_t now = KameoSteadyMilliseconds();
  int64_t previous = g_lastP2KeyboardTrace.load(std::memory_order_relaxed);
  if (now - previous >= 2000 && g_lastP2KeyboardTrace.compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
    REXLOG_INFO("[kameo] p2 route: user 1 stick={} player 2 pad={} players={} wanted={} bound={} connected={}", REX_LOAD_U8(0x82B354E1), REX_LOAD_U8(0x82732F6C), REX_LOAD_U32(0x82B718FC), REX_LOAD_U32(0x8273A880), REX_LOAD_U32(0x82732F70), REX_LOAD_U32(0x82732F78));
    const uint32_t p1_entry = REX_LOAD_U32(kPlayerList);
    const uint32_t p2_entry = REX_LOAD_U32(kPlayerList + 4);
    const uint32_t p1_actor = p1_entry ? REX_LOAD_U32(p1_entry + 16) : 0;
    const uint32_t p2_actor = p2_entry ? REX_LOAD_U32(p2_entry + 16) : 0;
    REXLOG_INFO("[kameo] player actors: p1={:08X} controls={:08X} p2={:08X} controls={:08X}", p1_actor, p1_actor ? REX_LOAD_U32(p1_actor + 2672) : 0, p2_actor, p2_actor ? REX_LOAD_U32(p2_actor + 2672) : 0);
    const uint32_t sticks = REX_LOAD_U32(kSticks);
    if (sticks >= 0x01000000u && sticks < 0xC0000000u) {
      const uint32_t p2_stick = sticks + 320 * REX_LOAD_U8(kPlayerControllers + 36);
      REXLOG_INFO("[kameo] p2 joy: buttons={:08X} held={:08X} move=({}, {})", REX_LOAD_U32(p2_stick), REX_LOAD_U32(p2_stick + 8), LoadFloat(base, p2_stick + 292), LoadFloat(base, p2_stick + 296));
    }
    REXLOG_INFO("[kameo] p2 keyboard poll: aggregate={} physical={} aggregate buttons={:04X} stick=({}, {}) physical buttons={:04X} stick=({}, {}) output buttons={:04X} stick=({}, {})", have_aggregate, have_device_keyboard, uint16_t(aggregate.gamepad.buttons), int16_t(aggregate.gamepad.thumb_lx), int16_t(aggregate.gamepad.thumb_ly), uint16_t(device_keyboard.gamepad.buttons), int16_t(device_keyboard.gamepad.thumb_lx), int16_t(device_keyboard.gamepad.thumb_ly), uint16_t(keyboard.gamepad.buttons), int16_t(keyboard.gamepad.thumb_lx), int16_t(keyboard.gamepad.thumb_ly));
  }
}

void TraceP2Controls(uint8_t* base, uint32_t player) {
  const uint32_t p2_entry = REX_LOAD_U32(kPlayerList + 4);
  if (player && p2_entry && player == REX_LOAD_U32(p2_entry + 16)) {
    const int64_t now = KameoSteadyMilliseconds();
    int64_t previous = g_lastP2ControlsTrace.load(std::memory_order_relaxed);
    if (now - previous >= 2000 && g_lastP2ControlsTrace.compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
      const uint32_t record = REX_LOAD_U32(player + 2672);
      if (record) REXLOG_INFO("[kameo] p2 controls: index={} buttons={:08X} held={:08X} move=({}, {}) mask={:08X}", REX_LOAD_U8(record + 14), REX_LOAD_U32(record + 44), REX_LOAD_U32(record + 48), LoadFloat(base, record + 60), LoadFloat(base, record + 64), REX_LOAD_U32(0x82732F3C));
    }
  }
}

}  // namespace kameo::input::detail

// _effectsWater_ControlUnderWaterStatusGameThread(effects) -- while split-screen is on, logs player two's underwater state (UnderWaterGFXState, the ray hit and its source) and probe ray whenever the state changes or every 5 s.
extern "C" REX_FUNC(sub_82287718) {
  __imp__sub_82287718(ctx, base);
  if (!REXCVAR_GET(kameo_story_splitscreen)) return;
  static int64_t last_trace = 0;
  static uint32_t last_state = ~0u;
  const uint32_t state = REX_LOAD_U32(0x8288CBC8 + 4);
  const int64_t now = KameoSteadyMilliseconds();
  if (state == last_state && now - last_trace < 5000) return;
  last_state = state;
  last_trace = now;
  PPCContext lookup = ctx;
  lookup.r3.u64 = 1;
  lookup.r4.u64 = 180012321;
  lookup.r5.u64 = 1;
  sub_822562C0(lookup, base);
  const uint32_t ray = lookup.r3.u32;
  if (ray) REXLOG_INFO("[kameo] underwater p2: state={} hit={} source={} ray=({}, {}, {})->({}, {}, {})", state, REX_LOAD_U32(0x8288BDB0 + 4), REX_LOAD_U32(0x8288CBD8 + 4), LoadFloat(base, ray), LoadFloat(base, ray + 4), LoadFloat(base, ray + 8), LoadFloat(base, ray + 12), LoadFloat(base, ray + 16), LoadFloat(base, ray + 20));
  else REXLOG_INFO("[kameo] underwater p2: state={} hit={} source={} ray unavailable", state, REX_LOAD_U32(0x8288BDB0 + 4), REX_LOAD_U32(0x8288CBD8 + 4));
}
