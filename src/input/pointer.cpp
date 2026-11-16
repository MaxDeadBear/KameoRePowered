// The mouse: when it looks around and when it is a pointer, and the pointer's position in the game's 640x480 menu layout.

#include <algorithm>
#include <cmath>
#include <mutex>

#include "core/cvar.h"
#include <rex/logging/log_category.h>

#include "core/time.h"
#include "gpu/graphics_system.h"
#include "gpu/replay/screen_layout.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"

namespace kameo::input::detail {

MouseListener g_mouse;
std::atomic<bool> g_mouseLook{true};
bool g_mouseCaptured = true;

namespace {

KameoMouseFrame g_mouseFrame;
uint32_t g_lastMoves = 0;
// Ticks since a selectable text window last took input; the cursor is freed while a menu is up.
uint32_t g_ticksSinceMenu = 1000;
uint32_t g_lastMenuTextObject = 0;
std::atomic<bool> g_imguiPointer{false};

// The pointer in the scene's own pixels, with the scene's size; a render size of another shape than the window is drawn centred with black bars. g_mouse.mutex must be held.
bool PointerInScene(float* x, float* y, uint32_t* width, uint32_t* height) {
  if (g_mouse.x < 0 || g_mouse.width == 0 || g_mouse.height == 0) return false;
  *x = float(g_mouse.x);
  *y = float(g_mouse.y);
  *width = g_mouse.width;
  *height = g_mouse.height;
  if (auto* gfx = kameo::gpu::GraphicsSystem::instance()) {
    float rect_x, rect_y, rect_width, rect_height;
    gfx->SceneRectInWindow(g_mouse.width, g_mouse.height, &rect_x, &rect_y, &rect_width, &rect_height, width, height);
    if (rect_width <= 0.0f || rect_height <= 0.0f || *width == 0 || *height == 0) return false;
    *x = (*x - rect_x) / rect_width * float(*width);
    *y = (*y - rect_y) / rect_height * float(*height);
  }
  return true;
}

}  // namespace

void MouseListener::OnInputEvent(rex::InputEvent& e) {
  using Type = rex::InputEvent::Type;
  switch (e.type) {
    case Type::kKeyDown:
      g_keyboardMouseUsed.store(KameoSteadyMilliseconds(), std::memory_order_relaxed);
      break;
    case Type::kMotion: {
      Record(e);
      std::lock_guard<std::mutex> lock(mutex);
      look_dx += e.dx;
      look_dy += e.dy;
      pointer_dx += e.dx;
      pointer_dy += e.dy;
      break;
    }
    case Type::kButtonDown: {
      g_keyboardMouseUsed.store(KameoSteadyMilliseconds(), std::memory_order_relaxed);
      Record(e);
      std::lock_guard<std::mutex> lock(mutex);
      if (e.button == rex::MouseButton::kLeft) {
        ++left_clicks;
        left_down = true;
      }
      if (e.button == rex::MouseButton::kRight) {
        ++right_clicks;
        right_down = true;
      }
      break;
    }
    case Type::kButtonUp: {
      std::lock_guard<std::mutex> lock(mutex);
      if (e.button == rex::MouseButton::kLeft) left_down = false;
      if (e.button == rex::MouseButton::kRight) right_down = false;
      break;
    }
    case Type::kWheel: {
      g_keyboardMouseUsed.store(KameoSteadyMilliseconds(), std::memory_order_relaxed);
      const int32_t travel = int32_t(std::lround(e.scroll_y * float(kScrollPerDetent)));
      std::lock_guard<std::mutex> lock(mutex);
      wheel += travel;
      trigger_wheel += travel;
      break;
    }
    case Type::kFocusLost: {
      std::lock_guard<std::mutex> lock(mutex);
      left_down = false;
      right_down = false;
      break;
    }
    default:
      break;
  }
}

void MouseListener::Record(const rex::InputEvent& e) {
  rex::hid::IInputSystem* input = InputSystem();
  if (!input) return;
  const rex::PointerState pointer = input->GetPointer();
  std::lock_guard<std::mutex> lock(mutex);
  // A locked pointer reports only relative motion, which counts as movement too.
  if (e.x != float(x) || e.y != float(y) || e.dx != 0.0f || e.dy != 0.0f) ++moves;
  x = int32_t(e.x);
  y = int32_t(e.y);
  width = pointer.window_width;
  height = pointer.window_height;
}

// No selectable menu and no player object is the "Press Start" title screen, or a load between screens.
bool AtTitleScreen() {
  uint8_t* base = GuestBase();
  return base && g_ticksSinceMenu > 3 && REX_LOAD_U32(kPlayerList) == 0;
}

}  // namespace kameo::input::detail

using namespace kameo::input::detail;

void KameoNoteMenuOpen(uint32_t text_object) {
  g_ticksSinceMenu = 0;
  g_lastMenuTextObject = text_object;
}

void KameoTickInput() {
  if (g_ticksSinceMenu < 1000) ++g_ticksSinceMenu;
  SyncInputMode();
  const bool keyboard_mouse = KameoKeyboardMouseMode();
  const bool menu_open = g_ticksSinceMenu <= 3 || AtTitleScreen();
  kameo::gpu::SetHudMenuOpen(menu_open);
  // The warrior wheel may put up text, but the mouse stays captured to point at a warrior.
  const bool look = keyboard_mouse && (!menu_open || ((WarriorWheelOpen(0) || WarriorWheelOpen(1)) && !AtTitleScreen()));
  // The Wotnot's wisp is the pointer on its pages, so the mouse is captured for it and the system cursor stays hidden, but clicks still count as menu presses.
  const bool book_pointer = keyboard_mouse && KameoBookPointerActive();
  // An open ImGui window frees the pointer over the game without leaving play, so bindings and prompts stay as they are.
  const bool imgui_pointer = g_imguiPointer.load(std::memory_order_relaxed);
  const bool capture = (look || book_pointer) && !imgui_pointer;
  g_mouseCaptured = capture;
  // The mouse is held in the window only when the player has turned mouse look on; the pointer is otherwise shown in menus and hidden in play.
  ApplyPointerMode(capture && kameo::cvar::GetBool("mnk_mouse"), (menu_open && !book_pointer) || imgui_pointer);
  if (look != g_mouseLook) {
    g_mouseLook = look;
    REXLOG_INFO("[kameo] input: mouse {} (menu text object {:08X}), mnk_mode {}, mnk_mouse {}", look ? "looks around" : "is a pointer", g_lastMenuTextObject, KameoKeyboardMouseMode() ? "on" : "off", kameo::cvar::GetText("mnk_mouse"));
  }
  {
    std::lock_guard<std::mutex> lock(g_mouse.mutex);
    KameoMouseFrame frame;
    float scene_x, scene_y;
    uint32_t scene_width, scene_height;
    frame.valid = PointerInScene(&scene_x, &scene_y, &scene_width, &scene_height) && kameo::gpu::WindowPointToUiLayout(scene_x, scene_y, scene_width, scene_height, &frame.x, &frame.y);
    frame.moved = g_mouse.moves != g_lastMoves;
    g_lastMoves = g_mouse.moves;
    // Clicks only count while the pointer is free, so a click that grabs the mouse for looking never also presses a menu line.
    // The pause menu keeps its pointer in Gamepad mode too, so the mode can always be switched back with the mouse.
    frame.left_clicks = look ? 0 : g_mouse.left_clicks;
    frame.right_clicks = look ? 0 : g_mouse.right_clicks;
    frame.wheel = look ? 0 : g_mouse.wheel;
    g_mouse.left_clicks = 0;
    g_mouse.right_clicks = 0;
    g_mouse.wheel = 0;
    // Pointer movement in a menu must not land on the camera as one big turn once play resumes.
    if (!look || imgui_pointer) {
      g_mouse.look_dx = 0.0f;
      g_mouse.look_dy = 0.0f;
    }
    if (!book_pointer || imgui_pointer) {
      g_mouse.pointer_dx = 0.0f;
      g_mouse.pointer_dy = 0.0f;
    }
    if (look) g_mouse.trigger_wheel = 0;
    g_mouseFrame = frame;
  }
  if (!look || imgui_pointer) {
    std::lock_guard<std::mutex> lock(g_physicalInputMutex);
    for (auto& mouse : g_playerMice) { mouse.dx = 0.0f; mouse.dy = 0.0f; }
  }
  ReleaseKeybindsWhenIdle();
}

const KameoMouseFrame& KameoMouseThisTick() { return g_mouseFrame; }

void KameoSetImGuiPointerWanted(bool wanted) { g_imguiPointer.store(wanted, std::memory_order_relaxed); }

bool KameoMouseWindowPoint(float* x, float* y) {
  std::lock_guard<std::mutex> lock(g_mouse.mutex);
  float scene_x, scene_y;
  uint32_t scene_width, scene_height;
  if (!PointerInScene(&scene_x, &scene_y, &scene_width, &scene_height)) return false;
  *x = scene_x / float(scene_width);
  *y = scene_y / float(scene_height);
  return true;
}

bool KameoMousePointerNow(float* layout_x, float* layout_y, uint32_t* moves) {
  std::lock_guard<std::mutex> lock(g_mouse.mutex);
  *moves = g_mouse.moves;
  if (g_mouse.x < 0) return false;
  float scene_x, scene_y;
  uint32_t scene_width, scene_height;
  return PointerInScene(&scene_x, &scene_y, &scene_width, &scene_height) && kameo::gpu::WindowPointToUiLayout(scene_x, scene_y, scene_width, scene_height, layout_x, layout_y);
}

void KameoConsumeMouseClicks() {
  g_mouseFrame.left_clicks = 0;
  g_mouseFrame.right_clicks = 0;
}

void KameoConsumeMouseWheel() { g_mouseFrame.wheel = 0; }
