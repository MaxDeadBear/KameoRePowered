// Which keyboards, mice and controllers reach which player: the runtime's device assignment, and SDL events sorted per physical device so two keyboards and two mice can serve two players.

#include <algorithm>
#include <cmath>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/logging/macros.h>
#include <rex/ui/sdl_virtual_key.h>
#include <rex/ui/window.h>

#include "app/process.h"
#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, kameo_story_splitscreen);
REXCVAR_DEFINE_INT32(kameo_input_mode, 0, "Kameo", "Player one: 0 = keyboard and mouse, 1 = first available gamepad.");
REXCVAR_DEFINE_INT32(kameo_p2_input_mode, 0, "Kameo", "0 = co-op gamepad, 1 = co-op keyboard and mouse; separate physical keyboards and mice can serve both players.");
REXCVAR_DEFINE_INT32(kameo_p1_keyboard_device, 0, "Kameo", "Physical keyboard number for player one (0 or 1).");
REXCVAR_DEFINE_INT32(kameo_p2_keyboard_device, 1, "Kameo", "Physical keyboard number for co-op (0 or 1).");
REXCVAR_DEFINE_INT32(kameo_p1_mouse_device, 0, "Kameo", "Physical mouse number for player one (0 or 1).");
REXCVAR_DEFINE_INT32(kameo_p2_mouse_device, 1, "Kameo", "Physical mouse number for co-op (0 or 1).");

namespace kameo::input::detail {

using rex::X_RESULT;
using rex::X_STATUS;

std::mutex g_physicalInputMutex;
std::array<SDL_KeyboardID, 2> g_physicalKeyboardIds{};
std::array<PhysicalMouseState, 2> g_playerMice;
std::atomic<uint32_t> g_padCount{0};

namespace {

std::array<SDL_MouseID, 2> g_physicalMouseIds{};
std::array<std::unique_ptr<rex::input::mnk::MnkInputDriver>, 2> g_physicalKeyboards;
bool g_inputFocused = true;
uint64_t g_routeSignature = 0;

class KameoDeviceAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    pads_.clear();
    fallback_ = rex::input::DeviceId::kInvalid;
    for (const auto& device : devices) {
      if (!device.synthetic) pads_.push_back(device.id);
      else if (device.id == static_cast<rex::input::DeviceId>(0x4E4F5000)) fallback_ = device.id;
    }
    g_padCount.store(uint32_t(pads_.size()), std::memory_order_relaxed);
  }

  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    const bool player_one_keyboard = REXCVAR_GET(kameo_input_mode) != 1;
    const bool coop = CoopInputRouteActive();
    if (user_index == 0) {
      if ((player_one_keyboard || pads_.empty()) && fallback_ != rex::input::DeviceId::kInvalid) out.push_back(fallback_);
      if (!pads_.empty() && (!player_one_keyboard || !coop)) out.push_back(pads_[0]);
    } else if (user_index == 1) {
      const bool player_two_keyboard = coop && REXCVAR_GET(kameo_p2_input_mode) == 1;
      if ((!coop || player_two_keyboard) && fallback_ != rex::input::DeviceId::kInvalid) out.push_back(fallback_);
      if (!player_two_keyboard) {
        const size_t pad_index = player_one_keyboard ? 0 : 1;
        if ((!player_one_keyboard || coop) && pad_index < pads_.size()) out.push_back(pads_[pad_index]);
      }
    }
  }

 private:
  std::vector<rex::input::DeviceId> pads_;
  rex::input::DeviceId fallback_ = rex::input::DeviceId::kInvalid;
};

uint64_t InputRouteSignature() {
  return uint64_t(REXCVAR_GET(kameo_input_mode) & 3) | (uint64_t(REXCVAR_GET(kameo_story_splitscreen)) << 2) | (uint64_t(REXCVAR_GET(kameo_p2_input_mode) & 3) << 3) | (uint64_t(REXCVAR_GET(kameo_p1_keyboard_device) & 3) << 5) | (uint64_t(REXCVAR_GET(kameo_p2_keyboard_device) & 3) << 7) | (uint64_t(REXCVAR_GET(kameo_p1_mouse_device) & 3) << 9) | (uint64_t(REXCVAR_GET(kameo_p2_mouse_device) & 3) << 11);
}

int PhysicalKeyboardSlot(int player) {
  const int selected = std::clamp(player == 0 ? REXCVAR_GET(kameo_p1_keyboard_device) : REXCVAR_GET(kameo_p2_keyboard_device), 0, 1);
  const bool both_keyboard = REXCVAR_GET(kameo_input_mode) != 1 && CoopInputRouteActive() && REXCVAR_GET(kameo_p2_input_mode) == 1;
  if (!both_keyboard && g_physicalKeyboardIds[size_t(selected)] == 0) return g_physicalKeyboardIds[size_t(1 - selected)] != 0 ? 1 - selected : 0;
  return selected;
}

int PhysicalMouseSlot(int player) {
  const int selected = std::clamp(player == 0 ? REXCVAR_GET(kameo_p1_mouse_device) : REXCVAR_GET(kameo_p2_mouse_device), 0, 1);
  const bool both_keyboard = REXCVAR_GET(kameo_input_mode) != 1 && CoopInputRouteActive() && REXCVAR_GET(kameo_p2_input_mode) == 1;
  if (!both_keyboard && g_physicalMouseIds[size_t(selected)] == 0) return g_physicalMouseIds[size_t(1 - selected)] != 0 ? 1 - selected : 0;
  return selected;
}

template <typename Id>
int DeviceSlot(std::array<Id, 2>& ids, Id id) {
  if (id == 0) return 0;
  for (int slot = 0; slot < 2; ++slot) if (ids[size_t(slot)] == id) return slot;
  for (int slot = 0; slot < 2; ++slot) if (ids[size_t(slot)] == 0) { ids[size_t(slot)] = id; return slot; }
  return -1;
}

int MousePlayerForSlot(int slot) {
  const bool first = REXCVAR_GET(kameo_input_mode) != 1;
  const bool second = CoopInputRouteActive() && REXCVAR_GET(kameo_p2_input_mode) == 1;
  if (first && second) return slot == PhysicalMouseSlot(0) ? 0 : slot == PhysicalMouseSlot(1) ? 1 : -1;
  return first && slot == PhysicalMouseSlot(0) ? 0 : second && slot == PhysicalMouseSlot(1) ? 1 : -1;
}

bool SDLCALL KameoPhysicalInputEvent(void*, SDL_Event* event) {
  std::lock_guard<std::mutex> lock(g_physicalInputMutex);
  if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
    const int slot = DeviceSlot(g_physicalKeyboardIds, event->key.which);
    if (slot >= 0 && g_physicalKeyboards[size_t(slot)]) {
      const rex::ui::VirtualKey vk = rex::ui::TranslateSDLScancode(event->key.scancode);
      if (vk != rex::ui::VirtualKey::kNone) {
        const SDL_Keymod mod = event->key.mod;
        rex::ui::KeyEvent key(nullptr, vk, 1, event->key.repeat, (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_ALT) != 0, (mod & SDL_KMOD_GUI) != 0);
        if (event->type == SDL_EVENT_KEY_DOWN) g_physicalKeyboards[size_t(slot)]->OnKeyDown(key);
        else g_physicalKeyboards[size_t(slot)]->OnKeyUp(key);
      }
    }
  } else if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_WHEEL) {
    const SDL_MouseID id = event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.which : event->type == SDL_EVENT_MOUSE_WHEEL ? event->wheel.which : event->button.which;
    const int slot = DeviceSlot(g_physicalMouseIds, id);
    const int player = slot >= 0 ? MousePlayerForSlot(slot) : -1;
    if (player >= 0) {
      if (event->type == SDL_EVENT_MOUSE_MOTION) {
        const SDL_Window* window = SDL_GetWindowFromID(event->motion.windowID);
        const float density = window ? SDL_GetWindowPixelDensity(const_cast<SDL_Window*>(window)) : 1.0f;
        g_playerMice[size_t(player)].dx += event->motion.xrel * density;
        g_playerMice[size_t(player)].dy += event->motion.yrel * density;
      } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        g_playerMice[size_t(player)].wheel += int32_t(std::lround(event->wheel.y * float(rex::ui::MouseEvent::kScrollPerDetent)));
      } else {
        const bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (event->button.button == SDL_BUTTON_LEFT) g_playerMice[size_t(player)].left = down;
        if (event->button.button == SDL_BUTTON_RIGHT) g_playerMice[size_t(player)].right = down;
        const int keyboard_slot = PhysicalKeyboardSlot(player);
        if (g_physicalKeyboards[size_t(keyboard_slot)]) {
          const auto button = event->button.button == SDL_BUTTON_LEFT ? rex::ui::MouseEvent::Button::kLeft : event->button.button == SDL_BUTTON_RIGHT ? rex::ui::MouseEvent::Button::kRight : event->button.button == SDL_BUTTON_MIDDLE ? rex::ui::MouseEvent::Button::kMiddle : rex::ui::MouseEvent::Button::kNone;
          rex::ui::MouseEvent mouse(nullptr, button, int32_t(event->button.x), int32_t(event->button.y));
          if (down) g_physicalKeyboards[size_t(keyboard_slot)]->OnMouseDown(mouse);
          else g_physicalKeyboards[size_t(keyboard_slot)]->OnMouseUp(mouse);
        }
      }
    }
  } else if (event->type == SDL_EVENT_KEYBOARD_REMOVED) {
    for (int slot = 0; slot < 2; ++slot) if (g_physicalKeyboardIds[size_t(slot)] == event->kdevice.which) {
      g_physicalKeyboardIds[size_t(slot)] = 0;
      rex::ui::UISetupEvent focus;
      g_physicalKeyboards[size_t(slot)]->OnLostFocus(focus);
      g_physicalKeyboards[size_t(slot)]->OnGotFocus(focus);
    }
  } else if (event->type == SDL_EVENT_MOUSE_REMOVED) {
    for (int slot = 0; slot < 2; ++slot) if (g_physicalMouseIds[size_t(slot)] == event->mdevice.which) {
      const int player = MousePlayerForSlot(slot);
      if (player >= 0) {
        g_playerMice[size_t(player)] = {};
        const int keyboard_slot = PhysicalKeyboardSlot(player);
        if (g_physicalKeyboards[size_t(keyboard_slot)]) for (const auto button : {rex::ui::MouseEvent::Button::kLeft, rex::ui::MouseEvent::Button::kRight, rex::ui::MouseEvent::Button::kMiddle}) {
          rex::ui::MouseEvent mouse(nullptr, button, 0, 0);
          g_physicalKeyboards[size_t(keyboard_slot)]->OnMouseUp(mouse);
        }
      }
      g_physicalMouseIds[size_t(slot)] = 0;
    }
  } else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
#ifdef _WIN32
    rex::ui::Window* input_window = kameo::app::MainWindow();
    SDL_Window* event_window = SDL_GetWindowFromID(event->window.windowID);
    if (!input_window || !event_window || SDL_GetPointerProperty(SDL_GetWindowProperties(event_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) != input_window->GetNativeWindowHandle()) return true;
#endif
    g_inputFocused = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
    rex::ui::UISetupEvent focus;
    for (auto& keyboard : g_physicalKeyboards) if (keyboard) {
      if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) keyboard->OnLostFocus(focus);
      else keyboard->OnGotFocus(focus);
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) g_playerMice = {};
  }
  return true;
}

}  // namespace

bool LevelRunning() {
  const uint8_t* base = GuestBase();
  return base && REX_LOAD_U32(kPlayerList) != 0;
}

bool CoopInputRouteActive() {
  return (REXCVAR_GET(kameo_story_splitscreen) && LevelRunning()) || KameoSteadyMilliseconds() - g_frontendP2JoinSeen.load(std::memory_order_relaxed) < 250;
}

void TakePhysicalMouseTravel(int player, float* dx, float* dy) {
  if (!(KameoPlayerUsesKeyboard(0) && KameoPlayerUsesKeyboard(1))) {
    std::lock_guard<std::mutex> lock(g_mouse.mutex);
    *dx = g_mouse.look_dx;
    *dy = g_mouse.look_dy;
    g_mouse.look_dx = 0.0f;
    g_mouse.look_dy = 0.0f;
    return;
  }
  std::lock_guard<std::mutex> lock(g_physicalInputMutex);
  *dx = g_playerMice[size_t(player)].dx;
  *dy = g_playerMice[size_t(player)].dy;
  g_playerMice[size_t(player)].dx = 0.0f;
  g_playerMice[size_t(player)].dy = 0.0f;
}

bool ReadPhysicalKeyboardState(int player, rex::input::X_INPUT_STATE* state) {
  std::lock_guard<std::mutex> lock(g_physicalInputMutex);
  const int slot = PhysicalKeyboardSlot(player);
  return g_physicalKeyboards[size_t(slot)] && g_physicalKeyboards[size_t(slot)]->GetDeviceState(kKeyboardDevice, state) == X_ERROR_SUCCESS;
}

void SyncInputMode() {
  const bool keyboard_mouse = KameoKeyboardMouseMode();
  if (REXCVAR_GET(mnk_mode) != keyboard_mouse) REXCVAR_SET(mnk_mode, keyboard_mouse);
  const uint64_t route = InputRouteSignature();
  if (route != g_routeSignature) {
    std::lock_guard<std::mutex> lock(g_physicalInputMutex);
    rex::ui::UISetupEvent focus;
    for (auto& keyboard : g_physicalKeyboards) if (keyboard) {
      keyboard->OnLostFocus(focus);
      if (g_inputFocused) keyboard->OnGotFocus(focus);
    }
    g_playerMice = {};
    g_standbyPadButtons.store(0, std::memory_order_relaxed);
    g_routeSignature = route;
  }
}

}  // namespace kameo::input::detail

using namespace kameo::input::detail;

void KameoInitInput(rex::ui::Window* window, rex::system::IInputSystem* input_system) {
  if (!window) return;
  if (input_system) static_cast<rex::input::InputSystem*>(input_system)->SetDeviceAssignment(std::make_unique<KameoDeviceAssignment>());
  kameo::app::SetMainWindow(window);
  g_routeSignature = InputRouteSignature();
  REXCVAR_SET(mnk_mode, REXCVAR_GET(kameo_input_mode) != 1 || (REXCVAR_GET(kameo_story_splitscreen) && REXCVAR_GET(kameo_p2_input_mode) == 1));
  for (auto& keyboard : g_physicalKeyboards) {
    keyboard = std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
    keyboard->Setup();
  }
  if (!SDL_AddEventWatch(KameoPhysicalInputEvent, nullptr)) REXLOG_WARN("[kameo] per-device keyboard and mouse input unavailable: {}", SDL_GetError());
#ifdef _WIN32
  InstallKameoCursor(window);
#endif
  window->AddInputListener(&g_mouse, 0);
  // Never freed: the process exits without running destructors, and detaching needs the UI thread.
  g_keyboard = new rex::input::mnk::MnkInputDriver(window, 1);
  if (g_keyboard->Setup() == X_STATUS_SUCCESS) {
    g_keyboard->OnWindowAvailable(window);
  } else {
    g_keyboard = nullptr;
  }
}
