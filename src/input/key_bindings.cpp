// Keyboard bindings: the PC defaults, holding them off while text is typed or a key is being bound, and binding a key from the Key Bindings page.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "core/settings_schema.h"
#include "input/input.h"
#include "input/input_internal.h"

namespace kameo::input::detail {

namespace {

// The runtime's keyboard driver bindings; their PC defaults (WASD, mouse buttons on the triggers, Space to jump) live in core/settings_schema.h with the rest of the settings.
constexpr const char* kKeybinds[] = {
    "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_trigger", "keybind_right_trigger",
    "keybind_left_shoulder", "keybind_right_shoulder", "keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right",
    "keybind_lstick_press", "keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right", "keybind_rstick_press",
    "keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right", "keybind_back", "keybind_start",
    "keybind_guide",
};

bool AnyKeyDown() {
#ifdef _WIN32
  for (int vk = 1; vk < 255; ++vk) {
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) continue;
    if ((::GetAsyncKeyState(vk) & 0x8000) != 0) return true;
  }
#endif
  return false;
}

std::mutex g_bindsMutex;
std::array<std::string, std::size(kKeybinds)> g_savedBinds;
bool g_bindsSuppressed = false;
int g_suppressRequests = 0;

constexpr auto kKeyCaptureTimeout = std::chrono::seconds(5);
std::mutex g_captureMutex;
std::string g_captureTarget;
std::chrono::steady_clock::time_point g_captureStarted;
bool g_captureDone = false;
bool g_captureCancelled = false;
std::string g_capturedKey;

void SetSuppressed(bool suppressed) {
  std::lock_guard<std::mutex> lock(g_bindsMutex);
  if (suppressed == g_bindsSuppressed) return;
  for (size_t i = 0; i < std::size(kKeybinds); ++i) {
    if (!rex::cvar::GetFlagInfo(kKeybinds[i])) continue;
    if (suppressed) {
      g_savedBinds[i] = rex::cvar::GetFlagByName(kKeybinds[i]);
      rex::cvar::SetFlagByName(kKeybinds[i], "");
    } else {
      rex::cvar::SetFlagByName(kKeybinds[i], g_savedBinds[i]);
    }
  }
  g_bindsSuppressed = suppressed;
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

bool SameKey(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

}  // namespace

void ReleaseKeybindsWhenIdle() {
  bool capturing;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    capturing = !g_captureTarget.empty();
  }
  if (!capturing && g_suppressRequests == 0 && g_bindsSuppressed && !AnyKeyDown()) SetSuppressed(false);
}

}  // namespace kameo::input::detail

using namespace kameo::input::detail;

void KameoApplyDefaultControls() {
  for (const char* name : kKeybinds) {
    const KameoSettingSpec* spec = KameoFindSetting(name);
    if (rex::cvar::GetFlagInfo(name) && rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, spec ? spec->default_value : "");
  }
  if (rex::cvar::GetFlagInfo("mnk_mouse") && rex::cvar::GetFlagSource("mnk_mouse") == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName("mnk_mouse", "true");
}

void KameoSuppressKeybinds(bool suppress) {
  g_suppressRequests = std::max(0, g_suppressRequests + (suppress ? 1 : -1));
  if (suppress) SetSuppressed(true);
}

std::string KameoKeybindForSave(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_bindsMutex);
  for (size_t i = 0; i < std::size(kKeybinds); ++i) {
    if (g_bindsSuppressed && name == kKeybinds[i]) return g_savedBinds[i];
  }
  return rex::cvar::GetFlagByName(name);
}

void KameoStartKeyCapture(const char* cvar) {
  SetSuppressed(true);
  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_captureTarget = cvar;
  g_captureStarted = std::chrono::steady_clock::now();
  g_captureDone = false;
  g_captureCancelled = false;
}

std::string KameoKeyCaptureTarget() {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  return g_captureTarget;
}

bool KameoKeyCaptureReady() {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  return !g_captureTarget.empty() && !g_captureDone && std::chrono::steady_clock::now() - g_captureStarted > std::chrono::milliseconds(300);
}

void KameoFeedKeyCapture(const char* key) {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  if (g_captureTarget.empty() || g_captureDone) return;
  g_captureDone = true;
  g_captureCancelled = key == nullptr;
  g_capturedKey = key ? key : "";
}

bool KameoTakeCapturedKey(std::string& cvar, std::string& key, bool& cancelled) {
  std::string target;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    // Every key can be bound, Escape included, so a capture nobody answers gives up on its own instead.
    if (!g_captureTarget.empty() && !g_captureDone && std::chrono::steady_clock::now() - g_captureStarted > kKeyCaptureTimeout) {
      g_captureDone = true;
      g_captureCancelled = true;
    }
    if (g_captureTarget.empty() || !g_captureDone) return false;
    target = g_captureTarget;
    cvar = g_captureTarget;
    key = g_capturedKey;
    cancelled = g_captureCancelled;
    g_captureTarget.clear();
    g_captureDone = false;
  }
  // The real bindings come back first, so the new key is written over the saved list rather than the blanked one.
  if (!cancelled) {
    std::lock_guard<std::mutex> lock(g_bindsMutex);
    const auto read = [](size_t i) { return g_bindsSuppressed ? g_savedBinds[i] : rex::cvar::GetFlagByName(kKeybinds[i]); };
    const auto write = [](size_t i, const std::string& keys) {
      if (g_bindsSuppressed) g_savedBinds[i] = keys;
      else rex::cvar::SetFlagByName(kKeybinds[i], keys);
    };
    std::vector<std::string> previous;
    for (size_t i = 0; i < std::size(kKeybinds); ++i) {
      if (target == kKeybinds[i]) previous = SplitKeys(read(i));
    }
    // A key already doing something else swaps over: that action takes this one's old keys in its place, so Start and Back trade Escape and Tab in one bind.
    for (size_t i = 0; i < std::size(kKeybinds); ++i) {
      if (target == kKeybinds[i]) continue;
      std::vector<std::string> keys = SplitKeys(read(i));
      const auto taken = std::find_if(keys.begin(), keys.end(), [&](const std::string& existing) { return SameKey(existing, key); });
      if (taken == keys.end()) continue;
      keys.erase(taken);
      for (const std::string& old_key : previous) {
        if (!SameKey(old_key, key) && std::none_of(keys.begin(), keys.end(), [&](const std::string& existing) { return SameKey(existing, old_key); })) keys.push_back(old_key);
      }
      std::string joined;
      for (const std::string& k : keys) joined += (joined.empty() ? "" : ",") + k;
      write(i, joined);
    }
    for (size_t i = 0; i < std::size(kKeybinds); ++i) {
      if (target == kKeybinds[i]) write(i, key);
    }
  }
  return true;
}
