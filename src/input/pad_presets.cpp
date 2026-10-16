// Controller presets and remapping: which physical control drives each game input, and capturing a new one from the Controller Bindings page.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>

#include <rex/cvar.h>

#include "input/input.h"
#include "input/input_internal.h"

REXCVAR_DECLARE(int32_t, kameo_input_mode);
REXCVAR_DEFINE_STRING(padbind_a, "face_bottom", "Kameo", "Physical controller input for A.");
REXCVAR_DEFINE_STRING(padbind_b, "face_right", "Kameo", "Physical controller input for B.");
REXCVAR_DEFINE_STRING(padbind_x, "face_left", "Kameo", "Physical controller input for X.");
REXCVAR_DEFINE_STRING(padbind_y, "face_top", "Kameo", "Physical controller input for Y.");
REXCVAR_DEFINE_STRING(padbind_left_shoulder, "left_shoulder", "Kameo", "Physical controller input for the left shoulder.");
REXCVAR_DEFINE_STRING(padbind_right_shoulder, "right_shoulder", "Kameo", "Physical controller input for the right shoulder.");
REXCVAR_DEFINE_STRING(padbind_left_trigger, "left_trigger", "Kameo", "Physical controller input for the left trigger.");
REXCVAR_DEFINE_STRING(padbind_right_trigger, "right_trigger", "Kameo", "Physical controller input for the right trigger.");
REXCVAR_DEFINE_STRING(padbind_left_stick_press, "left_stick_press", "Kameo", "Physical controller input for the left stick press.");
REXCVAR_DEFINE_STRING(padbind_right_stick_press, "right_stick_press", "Kameo", "Physical controller input for the right stick press.");
REXCVAR_DEFINE_STRING(padbind_dpad_up, "dpad_up", "Kameo", "Physical controller input for D-pad up.");
REXCVAR_DEFINE_STRING(padbind_dpad_down, "dpad_down", "Kameo", "Physical controller input for D-pad down.");
REXCVAR_DEFINE_STRING(padbind_dpad_left, "dpad_left", "Kameo", "Physical controller input for D-pad left.");
REXCVAR_DEFINE_STRING(padbind_dpad_right, "dpad_right", "Kameo", "Physical controller input for D-pad right.");
REXCVAR_DEFINE_STRING(padbind_start, "start", "Kameo", "Physical controller input for Start.");
REXCVAR_DEFINE_STRING(padbind_back, "back", "Kameo", "Physical controller input for Back.");
REXCVAR_DEFINE_STRING(padbind_left_up, "left_up", "Kameo", "Physical controller input for left stick up.");
REXCVAR_DEFINE_STRING(padbind_left_down, "left_down", "Kameo", "Physical controller input for left stick down.");
REXCVAR_DEFINE_STRING(padbind_left_left, "left_left", "Kameo", "Physical controller input for left stick left.");
REXCVAR_DEFINE_STRING(padbind_left_right, "left_right", "Kameo", "Physical controller input for left stick right.");
REXCVAR_DEFINE_STRING(padbind_right_up, "right_up", "Kameo", "Physical controller input for right stick up.");
REXCVAR_DEFINE_STRING(padbind_right_down, "right_down", "Kameo", "Physical controller input for right stick down.");
REXCVAR_DEFINE_STRING(padbind_right_left, "right_left", "Kameo", "Physical controller input for right stick left.");
REXCVAR_DEFINE_STRING(padbind_right_right, "right_right", "Kameo", "Physical controller input for right stick right.");

namespace kameo::input::detail {

namespace {

struct PadBinding {
  const char* cvar;
  const char* token;
  const char16_t* label;
  uint16_t button;
};

constexpr PadBinding kPadBindings[] = {
    {"padbind_a", "face_bottom", u"Face Bottom", rex::input::X_INPUT_GAMEPAD_A},
    {"padbind_b", "face_right", u"Face Right", rex::input::X_INPUT_GAMEPAD_B},
    {"padbind_x", "face_left", u"Face Left", rex::input::X_INPUT_GAMEPAD_X},
    {"padbind_y", "face_top", u"Face Top", rex::input::X_INPUT_GAMEPAD_Y},
    {"padbind_left_shoulder", "left_shoulder", u"Left Shoulder", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"padbind_right_shoulder", "right_shoulder", u"Right Shoulder", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"padbind_left_trigger", "left_trigger", u"Left Trigger", 0},
    {"padbind_right_trigger", "right_trigger", u"Right Trigger", 0},
    {"padbind_left_stick_press", "left_stick_press", u"Left Stick Press", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
    {"padbind_right_stick_press", "right_stick_press", u"Right Stick Press", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"padbind_dpad_up", "dpad_up", u"D-Pad Up", rex::input::X_INPUT_GAMEPAD_DPAD_UP},
    {"padbind_dpad_down", "dpad_down", u"D-Pad Down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
    {"padbind_dpad_left", "dpad_left", u"D-Pad Left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
    {"padbind_dpad_right", "dpad_right", u"D-Pad Right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
    {"padbind_start", "start", u"Start", rex::input::X_INPUT_GAMEPAD_START},
    {"padbind_back", "back", u"Back", rex::input::X_INPUT_GAMEPAD_BACK},
    {"padbind_left_up", "left_up", u"Left Stick Up", 0},
    {"padbind_left_down", "left_down", u"Left Stick Down", 0},
    {"padbind_left_left", "left_left", u"Left Stick Left", 0},
    {"padbind_left_right", "left_right", u"Left Stick Right", 0},
    {"padbind_right_up", "right_up", u"Right Stick Up", 0},
    {"padbind_right_down", "right_down", u"Right Stick Down", 0},
    {"padbind_right_left", "right_left", u"Right Stick Left", 0},
    {"padbind_right_right", "right_right", u"Right Stick Right", 0},
};

constexpr size_t kPadBindingCount = std::size(kPadBindings);
std::mutex g_padCaptureMutex;
std::string g_padCaptureTarget;
std::chrono::steady_clock::time_point g_padCaptureStarted;
int g_padCapturedSource = -1;
bool g_padCaptureArmed = false;
bool g_padCaptureDone = false;
bool g_padSuppressUntilRelease = false;
int32_t g_padCapturePreviousMode = 1;

int PadBindingIndex(const std::string& name) {
  for (size_t i = 0; i < kPadBindingCount; ++i) if (name == kPadBindings[i].cvar || name == kPadBindings[i].token) return int(i);
  return -1;
}

int PadSourceIndex(size_t destination) {
  const int source = PadBindingIndex(rex::cvar::GetFlagByName(kPadBindings[destination].cvar));
  return source < 0 ? int(destination) : source;
}

float PadSourceValue(const rex::input::X_INPUT_GAMEPAD& pad, size_t source) {
  if (kPadBindings[source].button) return (pad.buttons & kPadBindings[source].button) ? 1.0f : 0.0f;
  if (source == 6) return float(pad.left_trigger) / 255.0f;
  if (source == 7) return float(pad.right_trigger) / 255.0f;
  const int16_t axis = source < 20 ? (source < 18 ? pad.thumb_ly : pad.thumb_lx) : (source < 22 ? pad.thumb_ry : pad.thumb_rx);
  const bool positive = source == 16 || source == 19 || source == 20 || source == 23;
  return std::clamp(float(positive ? int32_t(axis) : -int32_t(axis)) / 32767.0f, 0.0f, 1.0f);
}

int16_t PadAxis(float positive, float negative) {
  return int16_t(std::lround(std::clamp(positive - negative, -1.0f, 1.0f) * 32767.0f));
}

}  // namespace

rex::input::X_INPUT_GAMEPAD RemapPad(const rex::input::X_INPUT_GAMEPAD& raw) {
  std::array<float, kPadBindingCount> value{};
  uint16_t remapped_buttons = 0;
  for (size_t i = 0; i < kPadBindingCount; ++i) {
    value[i] = PadSourceValue(raw, size_t(PadSourceIndex(i)));
    if (kPadBindings[i].button && value[i] > 0.5f) remapped_buttons |= kPadBindings[i].button;
  }
  rex::input::X_INPUT_GAMEPAD pad = raw;
  constexpr uint16_t kMappedButtons = 0xF3FF;
  pad.buttons = (raw.buttons & ~kMappedButtons) | remapped_buttons;
  pad.left_trigger = uint8_t(std::lround(value[6] * 255.0f));
  pad.right_trigger = uint8_t(std::lround(value[7] * 255.0f));
  pad.thumb_ly = PadAxis(value[16], value[17]);
  pad.thumb_lx = PadAxis(value[19], value[18]);
  pad.thumb_ry = PadAxis(value[20], value[21]);
  pad.thumb_rx = PadAxis(value[23], value[22]);
  return pad;
}

bool FeedPadCapture(const rex::input::X_INPUT_GAMEPAD& raw) {
  std::lock_guard<std::mutex> lock(g_padCaptureMutex);
  if (g_padCaptureTarget.empty() && !g_padSuppressUntilRelease) return false;
  uint32_t active = 0;
  for (size_t i = 0; i < kPadBindingCount; ++i) if (PadSourceValue(raw, i) > (i >= 16 ? 0.7f : 0.5f)) active |= 1u << i;
  if (g_padCaptureTarget.empty()) {
    if (!active) g_padSuppressUntilRelease = false;
    return active != 0;
  }
  if (!g_padCaptureArmed) g_padCaptureArmed = active == 0;
  else if (active && !g_padCaptureDone) {
    for (size_t i = 0; i < kPadBindingCount; ++i) if (active & (1u << i)) { g_padCapturedSource = int(i); break; }
    g_padCaptureDone = true;
  }
  return true;
}
}  // namespace kameo::input::detail

using namespace kameo::input::detail;

int KameoPadPreset() {
  bool positions = true;
  bool labels = true;
  for (size_t i = 0; i < kPadBindingCount; ++i) {
    const int source = PadSourceIndex(i);
    positions &= source == int(i);
    const int label_source = i < 4 ? int(i ^ 1) : int(i);
    labels &= source == label_source;
  }
  return positions ? 0 : labels ? 1 : 2;
}

void KameoSetPadPreset(int preset) {
  for (size_t i = 0; i < kPadBindingCount; ++i) {
    const size_t source = preset == 1 && i < 4 ? i ^ 1 : i;
    rex::cvar::SetFlagByName(kPadBindings[i].cvar, kPadBindings[source].token);
  }
}

std::u16string KameoPadBindingLabel(const char* cvar) {
  const int destination = PadBindingIndex(cvar);
  if (destination < 0) return u"Unknown";
  if (KameoPadCaptureTarget() == cvar) return u"Press a control (wait 8s to cancel)";
  return kPadBindings[PadSourceIndex(size_t(destination))].label;
}

void KameoStartPadCapture(const char* cvar) {
  if (PadBindingIndex(cvar) < 0) return;
  std::lock_guard<std::mutex> lock(g_padCaptureMutex);
  g_padCapturePreviousMode = REXCVAR_GET(kameo_input_mode);
  REXCVAR_SET(kameo_input_mode, 1);
  g_padCaptureTarget = cvar;
  g_padCaptureStarted = std::chrono::steady_clock::now();
  g_padCapturedSource = -1;
  g_padCaptureArmed = false;
  g_padCaptureDone = false;
}

std::string KameoPadCaptureTarget() {
  std::lock_guard<std::mutex> lock(g_padCaptureMutex);
  return g_padCaptureTarget;
}

bool KameoTakeCapturedPad(bool& cancelled) {
  std::string target;
  int source = -1;
  {
    std::lock_guard<std::mutex> lock(g_padCaptureMutex);
    if (g_padCaptureTarget.empty()) return false;
    const bool timed_out = std::chrono::steady_clock::now() - g_padCaptureStarted > std::chrono::seconds(8);
    if (!g_padCaptureDone && !timed_out) return false;
    target = g_padCaptureTarget;
    source = g_padCapturedSource;
    g_padCaptureTarget.clear();
    g_padCaptureDone = false;
    g_padSuppressUntilRelease = true;
    REXCVAR_SET(kameo_input_mode, g_padCapturePreviousMode);
  }
  cancelled = source < 0;
  if (cancelled) return true;
  const int destination = PadBindingIndex(target);
  if (destination < 0) return true;
  const int previous = PadSourceIndex(size_t(destination));
  for (size_t i = 0; i < kPadBindingCount; ++i) {
    if (int(i) != destination && PadSourceIndex(i) == source) rex::cvar::SetFlagByName(kPadBindings[i].cvar, kPadBindings[previous].token);
  }
  rex::cvar::SetFlagByName(target, kPadBindings[size_t(source)].token);
  return true;
}
