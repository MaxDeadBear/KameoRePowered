#include "ui/name_entry/name_entry_keys.h"

#include <string>
#include <utility>

#include "input/input.h"
#include "ui/name_entry/name_entry.h"

// The first key or mouse button pressed becomes the binding, named the way the runtime's keyboard driver parses them; waiting without pressing anything cancels.
void KameoNameEntryKeys::FeedKeyCapture() {
  static const std::pair<ImGuiKey, const char*> kNamed[] = {
      {ImGuiKey_Escape, "Escape"}, {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Return"}, {ImGuiKey_KeypadEnter, "NumpadEnter"}, {ImGuiKey_Tab, "Tab"}, {ImGuiKey_Backspace, "Backspace"},
      {ImGuiKey_Delete, "Delete"}, {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"}, {ImGuiKey_PageUp, "PageUp"}, {ImGuiKey_PageDown, "PageDown"},
      {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"},
      {ImGuiKey_Minus, "Minus"}, {ImGuiKey_Equal, "Plus"}, {ImGuiKey_Comma, "Comma"}, {ImGuiKey_Period, "Period"}, {ImGuiKey_Semicolon, "Semicolon"},
      {ImGuiKey_Slash, "Slash"}, {ImGuiKey_Backslash, "Backslash"}, {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_RightBracket, "RBracket"}, {ImGuiKey_Apostrophe, "Quote"},
      {ImGuiKey_KeypadAdd, "NumpadPlus"}, {ImGuiKey_KeypadSubtract, "NumpadMinus"}, {ImGuiKey_KeypadMultiply, "NumpadStar"}, {ImGuiKey_KeypadDivide, "NumpadSlash"}, {ImGuiKey_CapsLock, "CapsLock"},
  };
  for (const auto& [key, name] : kNamed) {
    if (ImGui::IsKeyPressed(key, false)) {
      KameoFeedKeyCapture(name);
      return;
    }
  }
  static std::string name;
  for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) {
      name = std::string(1, char('A' + (key - ImGuiKey_A)));
      KameoFeedKeyCapture(name.c_str());
      return;
    }
  }
  for (int key = ImGuiKey_0; key <= ImGuiKey_9; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) {
      name = std::string(1, char('0' + (key - ImGuiKey_0)));
      KameoFeedKeyCapture(name.c_str());
      return;
    }
  }
  for (int key = ImGuiKey_Keypad0; key <= ImGuiKey_Keypad9; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) {
      name = "Numpad" + std::to_string(key - ImGuiKey_Keypad0);
      KameoFeedKeyCapture(name.c_str());
      return;
    }
  }
  for (int key = ImGuiKey_F1; key <= ImGuiKey_F12; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) {
      name = "F" + std::to_string(1 + key - ImGuiKey_F1);
      KameoFeedKeyCapture(name.c_str());
      return;
    }
  }
  static const char* kMouseNames[] = {"LMB", "RMB", "MMB"};
  for (int button = 0; button < 3; ++button) {
    if (ImGui::IsMouseClicked(button)) {
      KameoFeedKeyCapture(kMouseNames[button]);
      return;
    }
  }
}

void KameoNameEntryKeys::OnDraw(ImGuiIO& io) {
  if (KameoKeyCaptureReady()) FeedKeyCapture();
  if (!KameoNameEntryActive()) return;
  // The window never starts SDL text input, so characters come from key presses rather than io.InputQueueCharacters.
  // ImGui's shift modifier never arrives here, so the modifiers are read from the OS.
  const bool shift = io.KeyShift || KameoKeyboardShiftHeld();
  const bool capitals = KameoKeyboardCapitals();
  for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), true)) KameoNameEntryPushKey(char32_t((capitals ? U'A' : U'a') + (key - ImGuiKey_A)));
  }
  for (int key = ImGuiKey_0; key <= ImGuiKey_9; ++key) {
    if (!ImGui::IsKeyPressed(ImGuiKey(key), true)) continue;
    if (shift && key == ImGuiKey_1) KameoNameEntryPushKey(U'!');
    else if (!shift) KameoNameEntryPushKey(char32_t(U'0' + (key - ImGuiKey_0)));
  }
  for (int key = ImGuiKey_Keypad0; key <= ImGuiKey_Keypad9; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), true)) KameoNameEntryPushKey(char32_t(U'0' + (key - ImGuiKey_Keypad0)));
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Space, true)) KameoNameEntryPushKey(U' ');
  if (ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true)) KameoNameEntryPushKey(U'-');
  if (ImGui::IsKeyPressed(ImGuiKey_Period, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, true)) KameoNameEntryPushKey(U'.');
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) KameoNameEntryPushKey(8);
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) KameoNameEntryPushKey(13);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) KameoNameEntryPushKey(27);
}
