// Keyboard, mouse and controller input: PC default bindings, the pointer in menus, co-op routing and suspending bindings while typing or binding a key.
#pragma once

#include <cstdint>
#include <string>

namespace rex::ui { class Window; }
namespace rex::system { class IInputSystem; }

struct KameoMouseFrame {
  bool valid = false;
  float x = 0.0f;
  float y = 0.0f;
  bool moved = false;
  int32_t left_clicks = 0;
  int32_t right_clicks = 0;
  int32_t wheel = 0;
};

void KameoInitInput(rex::ui::Window* window, rex::system::IInputSystem* input_system);
void KameoApplyDefaultControls();
void KameoTickInput();

bool KameoKeyboardMouseMode();
uint32_t KameoStandbyPadButtons();
int KameoKeyboardPlayer();
bool KameoPlayerUsesKeyboard(int player);
int KameoCoopInputMode();
void KameoSetCoopInputMode(int mode);
void KameoSetCoopEnabled(bool enabled);
bool KameoMenuCoop();
void KameoSetPlayerOneInputMode(int mode);

// True when typed letters should be capitals: Shift held or Caps Lock on, but not both.
bool KameoKeyboardCapitals();
bool KameoKeyboardShiftHeld();

// True while the Wotnot's wisp pointer is showing; the mouse drives it instead of pointing at menu lines.
bool KameoBookPointerActive();
// Whether an ImGui window that takes the mouse is on screen, set once per UI frame.
void KameoSetImGuiPointerWanted(bool wanted);
void KameoNoteMenuOpen(uint32_t text_object);
const KameoMouseFrame& KameoMouseThisTick();
// The pointer as it is right now in the 640x480 layout, with the listener's movement counter.
bool KameoMousePointerNow(float* layout_x, float* layout_y, uint32_t* moves);
// The pointer as a 0-1 fraction of the window.
bool KameoMouseWindowPoint(float* x, float* y);
void KameoConsumeMouseClicks();
void KameoConsumeMouseWheel();

void KameoSuppressKeybinds(bool suppress);
std::string KameoKeybindForSave(const std::string& name);
void KameoStartKeyCapture(const char* cvar);
std::string KameoKeyCaptureTarget();
bool KameoKeyCaptureReady();
void KameoFeedKeyCapture(const char* key);
bool KameoTakeCapturedKey(std::string& cvar, std::string& key, bool& cancelled);

int KameoPadPreset();
void KameoSetPadPreset(int preset);
std::u16string KameoPadBindingLabel(const char* cvar);
void KameoStartPadCapture(const char* cvar);
std::string KameoPadCaptureTarget();
bool KameoTakeCapturedPad(bool& cancelled);
