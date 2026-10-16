// How each pause-menu option reads and steps its setting.

#pragma once

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace kameo::ui {

// The game's Game Options settings, as setupMenuItemsForSettingsMenu reads them and storybookSimplePauseMenuPressACallback changes them; the per-player ones are 4 bytes apart.
constexpr uint32_t kIconPrefs = 0x8272F700;
constexpr uint32_t kScoreDisplay = 0x8272F710;
constexpr uint32_t kMapDisplay = 0x8272F730;
constexpr uint32_t kSplitScreenMode = 0x82755C08;
constexpr uint32_t kIsSplitScreenMode = 0x82B70AF8;
constexpr uint32_t kNumWantedPlayers = 0x8273A880;
constexpr uint32_t kHostPush = 0x8273310C;
constexpr uint32_t kHostAttack = 0x82B71B94;
constexpr uint32_t kScreenHeight = 0x8276696C;
// Game Options' Ortho Hints (Off, Adaptive, Helpful), and the hint calcCurrentHint picks for it.
constexpr uint32_t kOrthoHints = 0x828848F4;
constexpr uint32_t kCurrentHint = 0x828848E8;

std::u16string OnOff(bool on);
bool RunningVulkan();
// The renderer the next launch will use; automatic is whichever one is running now.
bool ChosenVulkan();
// The next value in `values` from the one nearest `current`; A wraps past the end, left and right stop at it.
int32_t Step(int32_t current, std::initializer_list<int32_t> values, int direction, bool wrap);
uint32_t GameSetting(uint32_t address);
void SetGameSetting(uint32_t address, uint32_t value);
// A 480-line split screen with the second layout keeps Control Display on its middle setting, greyed out.
bool ControlDisplayLocked();
// The next of the game's three-way settings, 0 to 2, in either direction.
uint32_t Cycle3(uint32_t value, int direction);
std::u16string LanguageName(int32_t language);
int32_t ChosenLanguage();
std::atomic<int32_t>* VolumeFor(uint32_t id);
// A binding row shows the keys bound to one controller input, and A waits for the next key or mouse button to replace them.
std::u16string BindingValue(const char* cvar);
std::u16string Sensitivity();
std::u16string ResolutionText();
void StepResolution(int direction, bool wrap);

}  // namespace kameo::ui
