// Which keyboards, mice and controllers reach which player: SDL events sorted per physical device so two keyboards and two mice can serve two players, and the controllers the runtime assigns to players.

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

#include "core/cvar.h"
#include <rex/logging/log_category.h>
#include <rex/module.h>
#include <rex/ui/ui_system.h>

#include "core/time.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "input/input_internal.h"

KCVAR_DECLARE(bool, kameo_story_splitscreen);
KCVAR_DEFINE_INT32(kameo_input_mode, 0, "Kameo", "Player one: 0 = keyboard and mouse, 1 = first available gamepad.");
KCVAR_DEFINE_INT32(kameo_p2_input_mode, 0, "Kameo", "0 = co-op gamepad, 1 = co-op keyboard and mouse; separate physical keyboards and mice can serve both players.");
KCVAR_DEFINE_INT32(kameo_p1_keyboard_device, 0, "Kameo", "Physical keyboard number for player one (0 or 1).");
KCVAR_DEFINE_INT32(kameo_p2_keyboard_device, 1, "Kameo", "Physical keyboard number for co-op (0 or 1).");
KCVAR_DEFINE_INT32(kameo_p1_mouse_device, 0, "Kameo", "Physical mouse number for player one (0 or 1).");
KCVAR_DEFINE_INT32(kameo_p2_mouse_device, 1, "Kameo", "Physical mouse number for co-op (0 or 1).");

namespace kameo::input::detail {

using rex::VirtualKey;

std::mutex g_physicalInputMutex;
std::array<SDL_KeyboardID, 2> g_physicalKeyboardIds{};
std::array<PhysicalMouseState, 2> g_playerMice;
std::atomic<uint32_t> g_padCount{0};

rex::hid::IInputSystem* InputSystem() { return REX_INTERFACE(rex::hid::IInputSystem); }

namespace {

std::array<SDL_MouseID, 2> g_physicalMouseIds{};
// The keys and mouse buttons each physical keyboard has down, with the mouse routed to its player.
std::array<HeldKeys, 2> g_physicalKeys;
std::atomic<bool> g_inputFocused{true};
uint64_t g_routeSignature = 0;
SDL_Window* g_sdlWindow = nullptr;
// Which of the four users the runtime reports a controller for.
std::atomic<uint32_t> g_connectedUsers{0};
// Never freed unless KameoShutdownInput runs: the process exits without running destructors, and detaching needs the UI thread.
rex::ScopeGuard* g_uiListener = nullptr;
rex::ScopeGuard* g_connectionListener = nullptr;
bool g_eventWatchAdded = false;

VirtualKey TranslateScancode(SDL_Scancode scancode) {
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) return static_cast<VirtualKey>(uint32_t(VirtualKey::kA) + (scancode - SDL_SCANCODE_A));
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) return static_cast<VirtualKey>(uint32_t(VirtualKey::k0) + 1 + (scancode - SDL_SCANCODE_1));
  if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) return static_cast<VirtualKey>(uint32_t(VirtualKey::kF1) + (scancode - SDL_SCANCODE_F1));
  if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) return static_cast<VirtualKey>(uint32_t(VirtualKey::kF13) + (scancode - SDL_SCANCODE_F13));
  // SDL's numpad runs 1 to 9 then 0, the virtual keys 0 to 9.
  if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) return static_cast<VirtualKey>(uint32_t(VirtualKey::kNumpad0) + 1 + (scancode - SDL_SCANCODE_KP_1));
  switch (scancode) {
    case SDL_SCANCODE_0: return VirtualKey::k0;
    case SDL_SCANCODE_KP_0: return VirtualKey::kNumpad0;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return VirtualKey::kReturn;
    case SDL_SCANCODE_ESCAPE: return VirtualKey::kEscape;
    case SDL_SCANCODE_BACKSPACE: return VirtualKey::kBack;
    case SDL_SCANCODE_TAB: return VirtualKey::kTab;
    case SDL_SCANCODE_SPACE: return VirtualKey::kSpace;
    case SDL_SCANCODE_LEFT: return VirtualKey::kLeft;
    case SDL_SCANCODE_UP: return VirtualKey::kUp;
    case SDL_SCANCODE_RIGHT: return VirtualKey::kRight;
    case SDL_SCANCODE_DOWN: return VirtualKey::kDown;
    case SDL_SCANCODE_PAGEUP: return VirtualKey::kPrior;
    case SDL_SCANCODE_PAGEDOWN: return VirtualKey::kNext;
    case SDL_SCANCODE_HOME: return VirtualKey::kHome;
    case SDL_SCANCODE_END: return VirtualKey::kEnd;
    case SDL_SCANCODE_INSERT: return VirtualKey::kInsert;
    case SDL_SCANCODE_DELETE: return VirtualKey::kDelete;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return VirtualKey::kShift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return VirtualKey::kControl;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return VirtualKey::kMenu;
    case SDL_SCANCODE_CAPSLOCK: return VirtualKey::kCapital;
    case SDL_SCANCODE_KP_MULTIPLY: return VirtualKey::kMultiply;
    case SDL_SCANCODE_KP_PLUS: return VirtualKey::kAdd;
    case SDL_SCANCODE_KP_MINUS: return VirtualKey::kSubtract;
    case SDL_SCANCODE_KP_PERIOD: return VirtualKey::kDecimal;
    case SDL_SCANCODE_KP_DIVIDE: return VirtualKey::kDivide;
    case SDL_SCANCODE_SEMICOLON: return VirtualKey::kOem1;
    case SDL_SCANCODE_EQUALS: return VirtualKey::kOemPlus;
    case SDL_SCANCODE_COMMA: return VirtualKey::kOemComma;
    case SDL_SCANCODE_MINUS: return VirtualKey::kOemMinus;
    case SDL_SCANCODE_PERIOD: return VirtualKey::kOemPeriod;
    case SDL_SCANCODE_SLASH: return VirtualKey::kOem2;
    case SDL_SCANCODE_GRAVE: return VirtualKey::kOem3;
    case SDL_SCANCODE_LEFTBRACKET: return VirtualKey::kOem4;
    case SDL_SCANCODE_BACKSLASH: return VirtualKey::kOem5;
    case SDL_SCANCODE_RIGHTBRACKET: return VirtualKey::kOem6;
    case SDL_SCANCODE_APOSTROPHE: return VirtualKey::kOem7;
    default: return VirtualKey::kNone;
  }
}

void SetHeld(HeldKeys& held, VirtualKey key, bool down) {
  const uint32_t code = uint32_t(key);
  if (code == 0 || code >= held.down.size()) return;
  held.down[code] = down;
  held.shift = held.down[uint32_t(VirtualKey::kShift)];
  held.ctrl = held.down[uint32_t(VirtualKey::kControl)];
  held.alt = held.down[uint32_t(VirtualKey::kMenu)];
}

uint64_t InputRouteSignature() {
  return uint64_t(KCVAR_GET(kameo_input_mode) & 3) | (uint64_t(KCVAR_GET(kameo_story_splitscreen)) << 2) | (uint64_t(KCVAR_GET(kameo_p2_input_mode) & 3) << 3) | (uint64_t(KCVAR_GET(kameo_p1_keyboard_device) & 3) << 5) | (uint64_t(KCVAR_GET(kameo_p2_keyboard_device) & 3) << 7) | (uint64_t(KCVAR_GET(kameo_p1_mouse_device) & 3) << 9) | (uint64_t(KCVAR_GET(kameo_p2_mouse_device) & 3) << 11);
}

int PhysicalKeyboardSlot(int player) {
  const int selected = std::clamp(player == 0 ? KCVAR_GET(kameo_p1_keyboard_device) : KCVAR_GET(kameo_p2_keyboard_device), 0, 1);
  const bool both_keyboard = KCVAR_GET(kameo_input_mode) != 1 && CoopInputRouteActive() && KCVAR_GET(kameo_p2_input_mode) == 1;
  if (!both_keyboard && g_physicalKeyboardIds[size_t(selected)] == 0) return g_physicalKeyboardIds[size_t(1 - selected)] != 0 ? 1 - selected : 0;
  return selected;
}

int PhysicalMouseSlot(int player) {
  const int selected = std::clamp(player == 0 ? KCVAR_GET(kameo_p1_mouse_device) : KCVAR_GET(kameo_p2_mouse_device), 0, 1);
  const bool both_keyboard = KCVAR_GET(kameo_input_mode) != 1 && CoopInputRouteActive() && KCVAR_GET(kameo_p2_input_mode) == 1;
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
  const bool first = KCVAR_GET(kameo_input_mode) != 1;
  const bool second = CoopInputRouteActive() && KCVAR_GET(kameo_p2_input_mode) == 1;
  if (first && second) return slot == PhysicalMouseSlot(0) ? 0 : slot == PhysicalMouseSlot(1) ? 1 : -1;
  return first && slot == PhysicalMouseSlot(0) ? 0 : second && slot == PhysicalMouseSlot(1) ? 1 : -1;
}

bool SDLCALL KameoPhysicalInputEvent(void*, SDL_Event* event) {
  std::lock_guard<std::mutex> lock(g_physicalInputMutex);
  if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
    const int slot = DeviceSlot(g_physicalKeyboardIds, event->key.which);
    if (slot >= 0) SetHeld(g_physicalKeys[size_t(slot)], TranslateScancode(event->key.scancode), event->type == SDL_EVENT_KEY_DOWN);
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
        g_playerMice[size_t(player)].wheel += int32_t(std::lround(event->wheel.y * float(kScrollPerDetent)));
      } else {
        const bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (event->button.button == SDL_BUTTON_LEFT) g_playerMice[size_t(player)].left = down;
        if (event->button.button == SDL_BUTTON_RIGHT) g_playerMice[size_t(player)].right = down;
        const VirtualKey button = event->button.button == SDL_BUTTON_LEFT ? VirtualKey::kLButton : event->button.button == SDL_BUTTON_RIGHT ? VirtualKey::kRButton : event->button.button == SDL_BUTTON_MIDDLE ? VirtualKey::kMButton : VirtualKey::kNone;
        SetHeld(g_physicalKeys[size_t(PhysicalKeyboardSlot(player))], button, down);
      }
    }
  } else if (event->type == SDL_EVENT_KEYBOARD_REMOVED) {
    for (int slot = 0; slot < 2; ++slot) if (g_physicalKeyboardIds[size_t(slot)] == event->kdevice.which) {
      g_physicalKeyboardIds[size_t(slot)] = 0;
      g_physicalKeys[size_t(slot)] = {};
    }
  } else if (event->type == SDL_EVENT_MOUSE_REMOVED) {
    for (int slot = 0; slot < 2; ++slot) if (g_physicalMouseIds[size_t(slot)] == event->mdevice.which) {
      const int player = MousePlayerForSlot(slot);
      if (player >= 0) {
        g_playerMice[size_t(player)] = {};
        HeldKeys& keys = g_physicalKeys[size_t(PhysicalKeyboardSlot(player))];
        for (const VirtualKey button : {VirtualKey::kLButton, VirtualKey::kRButton, VirtualKey::kMButton}) SetHeld(keys, button, false);
      }
      g_physicalMouseIds[size_t(slot)] = 0;
    }
  } else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
    if (!g_sdlWindow || SDL_GetWindowFromID(event->window.windowID) != g_sdlWindow) return true;
    g_inputFocused = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
    for (auto& keys : g_physicalKeys) keys = {};
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) g_playerMice = {};
  }
  return true;
}

}  // namespace

bool LevelRunning() {
  uint8_t* base = GuestBase();
  return base && REX_LOAD_U32(kPlayerList) != 0;
}

bool CoopInputRouteActive() {
  return (KCVAR_GET(kameo_story_splitscreen) && LevelRunning()) || KameoSteadyMilliseconds() - g_frontendP2JoinSeen.load(std::memory_order_relaxed) < 250;
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

bool ReadPhysicalKeyboardState(int player, rex::xam::X_INPUT_STATE* state) {
  HeldKeys held;
  {
    std::lock_guard<std::mutex> lock(g_physicalInputMutex);
    held = g_physicalKeys[size_t(PhysicalKeyboardSlot(player))];
  }
  *state = PadFromKeys(held);
  return true;
}

bool ReadAggregateKeyboardState(rex::xam::X_INPUT_STATE* state) {
  if (!InputSystem()) return false;
  // The keyboard reads neutral while the window has no focus.
  *state = g_inputFocused.load(std::memory_order_relaxed) ? PadFromKeys(SystemHeldKeys()) : rex::xam::X_INPUT_STATE{};
  return true;
}

void SyncInputMode() {
  // The runtime's own keyboard-and-mouse pad stays off: the keys are turned into controller states here, so a mnk_mode left on by a config would press everything twice.
  if (kameo::cvar::Exists("mnk_mode") && kameo::cvar::GetBool("mnk_mode")) kameo::cvar::SetText("mnk_mode", "false");
  const uint64_t route = InputRouteSignature();
  if (route != g_routeSignature) {
    std::lock_guard<std::mutex> lock(g_physicalInputMutex);
    for (auto& keys : g_physicalKeys) keys = {};
    g_playerMice = {};
    g_standbyPadButtons.store(0, std::memory_order_relaxed);
    g_routeSignature = route;
  }
}

}  // namespace kameo::input::detail

using namespace kameo::input::detail;

void KameoInitInput() {
  rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem);
  rex::hid::IInputSystem* input = InputSystem();
  if (!ui || !input) return;
  if (const auto native = ui->GetNativeWindow()) g_sdlWindow = static_cast<SDL_Window*>(native->sdl);
  g_routeSignature = InputRouteSignature();
  SyncInputMode();
  if (!g_eventWatchAdded) {
    g_eventWatchAdded = SDL_AddEventWatch(KameoPhysicalInputEvent, nullptr);
    if (!g_eventWatchAdded) REXLOG_WARN("[kameo] per-device keyboard and mouse input unavailable: {}", SDL_GetError());
  }
#ifdef _WIN32
  InstallKameoCursor();
#endif
  if (!g_uiListener) g_uiListener = new rex::ScopeGuard(ui->AddEventListener(g_mouse));
  // The controllers the runtime has assigned to players; it reports a change when the game reads input.
  if (!g_connectionListener) g_connectionListener = new rex::ScopeGuard(input->OnConnectionChange([](uint32_t user, bool connected) {
    if (user >= rex::hid::IInputSystem::kMaxUsers) return;
    const uint32_t bit = 1u << user;
    const uint32_t users = connected ? g_connectedUsers.fetch_or(bit) | bit : g_connectedUsers.fetch_and(~bit) & ~bit;
    g_padCount.store(uint32_t(std::popcount(users)), std::memory_order_relaxed);
  }));
}

void KameoShutdownInput() {
  if (g_uiListener) g_uiListener->Reset();
  if (g_connectionListener) g_connectionListener->Reset();
  delete g_uiListener;
  delete g_connectionListener;
  g_uiListener = nullptr;
  g_connectionListener = nullptr;
  if (g_eventWatchAdded) SDL_RemoveEventWatch(KameoPhysicalInputEvent, nullptr);
  g_eventWatchAdded = false;
  g_sdlWindow = nullptr;
}
