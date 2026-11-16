// State shared by the keyboard, mouse and controller files.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <SDL3/SDL.h>

#include <rex/hid/input_system.h>
#include <rex/ui/ui_event.h>
#include <rex/xam/input.h>

namespace kameo::input::detail {

// Wheel travel is counted in 120ths of a detent, as the game's menus count it.
constexpr int32_t kScrollPerDetent = 120;

// The mouse as the game window's input events show it.
class MouseListener final : public rex::ui::IUIEventListener {
 public:
  void OnUIEvent(const rex::ui::UIEvent&) override {}
  void OnInputEvent(rex::InputEvent& e) override;
  void Record(const rex::InputEvent& e);

  std::mutex mutex;
  int32_t x = -1;
  int32_t y = -1;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t moves = 0;
  int32_t left_clicks = 0;
  int32_t right_clicks = 0;
  int32_t wheel = 0;
  // Wheel travel not yet turned into menu trigger presses.
  int32_t trigger_wheel = 0;
  bool left_down = false;
  bool right_down = false;
  // Mouse travel since the camera last took it.
  float look_dx = 0.0f;
  float look_dy = 0.0f;
  // Mouse travel since the Wotnot's book pointer last took it.
  float pointer_dx = 0.0f;
  float pointer_dy = 0.0f;
};

struct PhysicalMouseState {
  float dx = 0.0f;
  float dy = 0.0f;
  int32_t wheel = 0;
  bool left = false;
  bool right = false;
};

// The input system; null before the app's modules connect and after shutdown.
rex::hid::IInputSystem* InputSystem();
constexpr float kRadiansPerPixel = 0.0025f;

// pointer.cpp
extern MouseListener g_mouse;
// Whether the mouse turns the camera (rather than being a menu pointer), and whether it is captured for that or for the book pointer.
extern std::atomic<bool> g_mouseLook;
extern bool g_mouseCaptured;
// No selectable menu and no player object is the "Press Start" title screen, or a load between screens.
bool AtTitleScreen();

// device_routing.cpp
extern std::mutex g_physicalInputMutex;
extern std::array<SDL_KeyboardID, 2> g_physicalKeyboardIds;
extern std::array<PhysicalMouseState, 2> g_playerMice;
// Controllers connected, so split-screen knows whether each player can have one.
extern std::atomic<uint32_t> g_padCount;
// A level with a player in it; elsewhere there is no player two to route to, so player one takes every device.
bool LevelRunning();
bool CoopInputRouteActive();
void TakePhysicalMouseTravel(int player, float* dx, float* dy);
bool ReadPhysicalKeyboardState(int player, rex::xam::X_INPUT_STATE* state);
// The keyboard and mouse reach the game through the pad conversion in key_bindings.cpp, so the runtime's own keyboard-and-mouse pad stays off whatever a config says.
void SyncInputMode();

// player_join.cpp
extern std::atomic<int64_t> g_frontendP2JoinSeen;
// When the keyboard or mouse and when each player's controller were last pressed, so each player's controls follow what they touch.
extern std::atomic<int64_t> g_keyboardMouseUsed;
extern std::array<std::atomic<int64_t>, 2> g_padUsed;
void SuppressPlayerOneActivation(uint32_t user, rex::xam::X_INPUT_GAMEPAD* gamepad);
void ChooseControlsByUse();

// pad_presets.cpp
rex::xam::X_INPUT_GAMEPAD RemapPad(const rex::xam::X_INPUT_GAMEPAD& raw);
// Feeds a controller capture; true while the pad's input must be kept from the game.
bool FeedPadCapture(const rex::xam::X_INPUT_GAMEPAD& raw);

// key_bindings.cpp
// Which keys and mouse buttons are down, as virtual-key flags, and which modifiers are held.
struct HeldKeys {
  std::array<bool, 256> down{};
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
};
// The 25 key bindings turned into a controller state for the keys held.
rex::xam::X_INPUT_STATE PadFromKeys(const HeldKeys& held);
// The keys the runtime's input system reports down, all keyboards together.
HeldKeys SystemHeldKeys();
// Puts the bindings back once nothing asks for them to be held off and no key is down.
void ReleaseKeybindsWhenIdle();

// keyboard_mouse.cpp
// All keyboards and mice together as a controller state, apart from the controllers the runtime merges into player one.
bool ReadAggregateKeyboardState(rex::xam::X_INPUT_STATE* state);
extern std::atomic<uint32_t> g_standbyPadButtons;
extern std::array<uint32_t, 2> g_padPolls;
extern std::array<uint32_t, 2> g_mouseLookPoll;

// mouse_aim.cpp
bool WarriorWheelOpen(int player);

// win32_cursor.cpp
// Holds the pointer in the window (captured), or shows it (menu_open) or hides it over the window.
void ApplyPointerMode(bool captured, bool menu_open);
#ifdef _WIN32
void InstallKameoCursor();
#endif

}  // namespace kameo::input::detail
