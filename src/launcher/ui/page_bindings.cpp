// Key Bindings page: every keyboard action, rebound by pressing the new key, with the pause menu's swap when a key is taken.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {
namespace {

struct BindRow {
  const char* label;
  const char* name;
};

constexpr BindRow kBindRows[] = {
    {"Move Forward", "keybind_lstick_up"}, {"Move Back", "keybind_lstick_down"}, {"Move Left", "keybind_lstick_left"}, {"Move Right", "keybind_lstick_right"},
    {"A (Jump)", "keybind_a"}, {"B", "keybind_b"}, {"X", "keybind_x"}, {"Y", "keybind_y"},
    {"Left Trigger", "keybind_left_trigger"}, {"Right Trigger", "keybind_right_trigger"}, {"Left Bumper", "keybind_left_shoulder"}, {"Right Bumper", "keybind_right_shoulder"},
    {"Camera Up", "keybind_rstick_up"}, {"Camera Down", "keybind_rstick_down"}, {"Camera Left", "keybind_rstick_left"}, {"Camera Right", "keybind_rstick_right"},
    {"Left Stick Click", "keybind_lstick_press"}, {"Right Stick Click", "keybind_rstick_press"},
    {"D-Pad Up", "keybind_dpad_up"}, {"D-Pad Down", "keybind_dpad_down"}, {"D-Pad Left", "keybind_dpad_left"}, {"D-Pad Right", "keybind_dpad_right"},
    {"Start", "keybind_start"}, {"Back", "keybind_back"},
};

constexpr std::chrono::milliseconds kCaptureArmDelay(300);
constexpr std::chrono::seconds kCaptureTimeout(5);

const char* PressedKeyName() {
  static const std::pair<ImGuiKey, const char*> kNamed[] = {
      {ImGuiKey_Escape, "Escape"}, {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Return"}, {ImGuiKey_KeypadEnter, "NumpadEnter"}, {ImGuiKey_Tab, "Tab"}, {ImGuiKey_Backspace, "Backspace"},
      {ImGuiKey_Delete, "Delete"}, {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"}, {ImGuiKey_PageUp, "PageUp"}, {ImGuiKey_PageDown, "PageDown"},
      {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"},
      {ImGuiKey_Minus, "Minus"}, {ImGuiKey_Equal, "Plus"}, {ImGuiKey_Comma, "Comma"}, {ImGuiKey_Period, "Period"}, {ImGuiKey_Semicolon, "Semicolon"},
      {ImGuiKey_Slash, "Slash"}, {ImGuiKey_Backslash, "Backslash"}, {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_RightBracket, "RBracket"}, {ImGuiKey_Apostrophe, "Quote"},
      {ImGuiKey_KeypadAdd, "NumpadPlus"}, {ImGuiKey_KeypadSubtract, "NumpadMinus"}, {ImGuiKey_KeypadMultiply, "NumpadStar"}, {ImGuiKey_KeypadDivide, "NumpadSlash"}, {ImGuiKey_CapsLock, "CapsLock"},
  };
  for (const auto& [key, name] : kNamed) {
    if (ImGui::IsKeyPressed(key, false)) return name;
  }
  static std::string name;
  for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) return (name = std::string(1, char('A' + (key - ImGuiKey_A)))).c_str();
  }
  for (int key = ImGuiKey_0; key <= ImGuiKey_9; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) return (name = std::string(1, char('0' + (key - ImGuiKey_0)))).c_str();
  }
  for (int key = ImGuiKey_Keypad0; key <= ImGuiKey_Keypad9; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) return (name = "Numpad" + std::to_string(key - ImGuiKey_Keypad0)).c_str();
  }
  for (int key = ImGuiKey_F1; key <= ImGuiKey_F12; ++key) {
    if (ImGui::IsKeyPressed(ImGuiKey(key), false)) return (name = "F" + std::to_string(1 + key - ImGuiKey_F1)).c_str();
  }
  static const char* kMouseNames[] = {"LMB", "RMB", "MMB"};
  for (int button = 0; button < 3; ++button) {
    if (ImGui::IsMouseClicked(button)) return kMouseNames[button];
  }
  return nullptr;
}

std::vector<std::string> SplitKeys(const std::string& keys) {
  std::vector<std::string> tokens;
  size_t start = 0;
  while (start <= keys.size()) {
    size_t end = keys.find(',', start);
    if (end == std::string::npos) end = keys.size();
    std::string token = keys.substr(start, end - start);
    token.erase(0, token.find_first_not_of(' '));
    token.erase(token.find_last_not_of(' ') + 1);
    if (!token.empty()) tokens.push_back(token);
    start = end + 1;
  }
  return tokens;
}

bool SameKey(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

// The same swap the pause menu makes: an action that already had the key takes the rebound action's old keys instead.
void Bind(App& app, const std::string& target, const std::string& key) {
  const std::vector<std::string> previous = SplitKeys(app.settings.Get(target));
  for (const BindRow& row : kBindRows) {
    if (target == row.name) continue;
    std::vector<std::string> keys = SplitKeys(app.settings.Get(row.name));
    const auto taken = std::find_if(keys.begin(), keys.end(), [&](const std::string& existing) { return SameKey(existing, key); });
    if (taken == keys.end()) continue;
    keys.erase(taken);
    for (const std::string& old_key : previous) {
      if (!SameKey(old_key, key) && std::none_of(keys.begin(), keys.end(), [&](const std::string& existing) { return SameKey(existing, old_key); })) keys.push_back(old_key);
    }
    std::string joined;
    for (const std::string& k : keys) joined += (joined.empty() ? "" : ",") + k;
    app.settings.Set(row.name, joined);
  }
  app.settings.Set(target, key);
}

}  // namespace

void PollCapture(App& app) {
  if (app.capture_target.empty()) return;
  const auto elapsed = std::chrono::steady_clock::now() - app.capture_started;
  if (elapsed > kCaptureTimeout) {
    app.capture_target.clear();
    return;
  }
  if (elapsed < kCaptureArmDelay) return;
  if (const char* key = PressedKeyName()) {
    Bind(app, app.capture_target, key);
    app.capture_target.clear();
  }
}

void PageBindings(App& app) {
  PageTitle(app, "Key Bindings", "Click Change, then press the key or mouse button to use. Binding a key another action has swaps the two.");
  std::vector<const char*> names;
  for (const BindRow& row : kBindRows) names.push_back(row.name);
  const float width = ImGui::CalcTextSize("Restore Defaults").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
  if (ImGui::Button("Restore Defaults")) {
    for (const char* name : names) app.settings.Reset(name);
    app.capture_target.clear();
  }
  ImGui::BeginChild("bindings", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  if (ImGui::BeginTable("bind_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    for (const BindRow& row : kBindRows) {
      ImGui::PushID(row.name);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(row.label);
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      if (app.capture_target == row.name) {
        const int left = int(std::ceil(std::chrono::duration<float>(kCaptureTimeout - (std::chrono::steady_clock::now() - app.capture_started)).count()));
        ImGui::TextColored(kGold, "Press a key... (%ds)", std::max(left, 0));
      } else {
        const std::string keys = app.settings.Get(row.name);
        ImGui::TextUnformatted(keys.empty() ? "Unbound" : keys.c_str());
      }
      ImGui::TableSetColumnIndex(2);
      if (ImGui::Button("Change")) {
        app.capture_target = row.name;
        app.capture_started = std::chrono::steady_clock::now();
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset")) app.settings.Reset(row.name);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

}  // namespace kameo::launcher
