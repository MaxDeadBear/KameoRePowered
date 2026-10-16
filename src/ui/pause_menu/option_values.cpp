#include "ui/pause_menu/option_values.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <rex/cvar.h>

#include "core/settings.h"
#include "engine/audio.h"
#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "input/input.h"
#include "ui/pause_menu/pause_menu_internal.h"

REXCVAR_DECLARE(int32_t, kameo_language);
REXCVAR_DECLARE(double, mnk_sensitivity);

namespace kameo::ui {

namespace {

struct Resolution {
  int32_t width;
  int32_t height;
};

// The window's own size first, then 16:9 from 360p to 4K and the 21:9 ultrawide sizes; the renderer shows one of another shape than the window with black bars.
std::vector<Resolution> ResolutionChoices(int32_t* window_width, int32_t* window_height) {
  uint32_t width = 0;
  uint32_t height = 0;
  if (auto* gfx = kameo::gpu::GraphicsSystem::instance()) gfx->WindowSize(&width, &height);
  *window_width = int32_t(width);
  *window_height = int32_t(height);
  return {{0, 0}, {640, 360}, {854, 480}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {2560, 1080}, {3440, 1440}, {5120, 2160}};
}

// The choice the settings are on, or the one with the nearest height when the window has changed shape since it was picked.
size_t CurrentResolution(const std::vector<Resolution>& choices) {
  const int32_t height = REXCVAR_GET(kameo_gfx_render_height);
  const int32_t width = REXCVAR_GET(kameo_gfx_render_width);
  if (height <= 0) return 0;
  size_t best = 1;
  for (size_t i = 1; i < choices.size(); ++i) {
    if (choices[i].height == height && (choices[i].width == width || width <= 0)) return i;
    if (std::abs(choices[i].height - height) < std::abs(choices[best].height - height)) best = i;
  }
  return std::min(best, choices.size() - 1);
}

}  // namespace

std::u16string OnOff(bool on) { return on ? u"On" : u"Off"; }

bool RunningVulkan() {
  auto* gfx = kameo::gpu::GraphicsSystem::instance();
  return gfx && gfx->using_vulkan();
}

bool ChosenVulkan() {
  const std::string& want = REXCVAR_GET(kameo_gfx_backend);
  return want == "vulkan" || (want != "d3d12" && RunningVulkan());
}

int32_t Step(int32_t current, std::initializer_list<int32_t> values, int direction, bool wrap) {
  const std::vector<int32_t> list(values);
  size_t index = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    if (std::abs(list[i] - current) < std::abs(list[index] - current)) index = i;
  }
  const int32_t next = int32_t(index) + direction;
  if (next < 0) return wrap ? list.back() : list.front();
  if (next >= int32_t(list.size())) return wrap ? list.front() : list.back();
  return list[size_t(next)];
}

uint32_t GameSetting(uint32_t address) {
  const uint8_t* base = GuestBase();
  return REX_LOAD_U32(address);
}

void SetGameSetting(uint32_t address, uint32_t value) {
  uint8_t* base = GuestBase();
  REX_STORE_U32(address, value);
}

bool ControlDisplayLocked() {
  const uint8_t* base = GuestBase();
  return REX_LOAD_U16(kScreenHeight) == 480 && REX_LOAD_U32(kIsSplitScreenMode) != 0 && REX_LOAD_U32(kSplitScreenMode) == 1;
}

uint32_t Cycle3(uint32_t value, int direction) { return (value + (direction < 0 ? 2 : 1)) % 3; }

std::u16string LanguageName(int32_t language) {
  static const char16_t* kNames[] = {u"English", u"English", u"Japanese", u"German", u"French", u"Spanish", u"Italian", u"Korean", u"Chinese"};
  return kNames[language >= 1 && language <= 8 ? language : 1];
}

int32_t ChosenLanguage() {
  const int32_t language = REXCVAR_GET(kameo_language);
  return language >= 1 && language <= 8 ? language : int32_t(KameoRunningLanguage());
}

int32_t GammaBarStep() { return std::clamp(int32_t(std::lround((REXCVAR_GET(kameo_gfx_user_gamma) - 50) * kGammaBarSegments / 150.0)), 0, kGammaBarSegments); }

void SetGammaBarStep(int32_t step) { REXCVAR_SET(kameo_gfx_user_gamma, 50 + int32_t(std::lround(std::clamp(step, 0, kGammaBarSegments) * 150.0 / kGammaBarSegments))); }

std::atomic<int32_t>* VolumeFor(uint32_t id) {
  switch (id) {
    case 0x7A5E0101: return &g_kameo_master_volume;
    case 0x7A5E0102: return &g_kameo_music_volume;
    case 0x7A5E0103: return &g_kameo_sfx_volume;
    case 0x7A5E0104: return &g_kameo_ambience_volume;
    case 0x7A5E0105: return &g_kameo_speech_volume;
    default: return &g_kameo_fmv_volume;
  }
}

std::u16string BindingValue(const char* cvar) {
  if (KameoKeyCaptureTarget() == cvar) return u"Press a key (wait 5s to cancel)";
  const std::string keys = KameoKeybindForSave(cvar);
  if (keys.empty()) return u"Unbound";
  std::u16string text;
  for (char c : keys) {
    text += char16_t(c);
    if (c == ',') text += u' ';
  }
  return text;
}

std::u16string Sensitivity() {
  const int32_t tenths = int32_t(std::lround(REXCVAR_GET(mnk_sensitivity) * 10.0));
  return Number(tenths / 10, u".") + Number(tenths % 10, u"");
}

std::u16string ResolutionText() {
  int32_t window_width = 0;
  int32_t window_height = 0;
  const std::vector<Resolution> choices = ResolutionChoices(&window_width, &window_height);
  const Resolution& choice = choices[CurrentResolution(choices)];
  if (!window_width || !window_height) return choice.height <= 0 ? std::u16string(u"Window") : Number(choice.height, u"p");
  if (choice.height <= 0) return u"Window (" + Number(window_width, u"x") + Number(window_height, u")");
  return Number(choice.width, u"x") + Number(choice.height, u"");
}

void StepResolution(int direction, bool wrap) {
  int32_t window_width = 0;
  int32_t window_height = 0;
  const std::vector<Resolution> choices = ResolutionChoices(&window_width, &window_height);
  int32_t next = int32_t(CurrentResolution(choices)) + direction;
  if (next < 0) next = wrap ? int32_t(choices.size()) - 1 : 0;
  if (next >= int32_t(choices.size())) next = wrap ? 0 : int32_t(choices.size()) - 1;
  REXCVAR_SET(kameo_gfx_render_height, choices[size_t(next)].height);
  REXCVAR_SET(kameo_gfx_render_width, choices[size_t(next)].width);
}

}  // namespace kameo::ui
