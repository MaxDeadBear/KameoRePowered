// Shared state of the pause and options menu, which is built into the game's own text-window menu.

#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "ui/text_window.h"

namespace kameo::ui {

constexpr uint32_t kNextTextItemWriteOverPreviousLine = 0x82B713B3;
// storybookSimplePauseMenuPressACallback, which every one of our selectable lines uses.
constexpr uint32_t kPauseMenuPressACallback = 0x823F1078;
// The settings page's B handler, which returns to the main pause page when the menu was opened from gameplay.
constexpr uint32_t kSettingsBackCallback = 0x823F1028;
// storybookSimplePauseQuitMenuPressBCallback, the quit prompt's B handler.
constexpr uint32_t kQuitBackCallback = 0x823F1830;
constexpr uint32_t kReturnToLobbyCallback = 0x823F17F0;
// Localised layout strings the settings page itself uses for a label and its value on one row.
constexpr int32_t kLabelFormatString = 588;
constexpr int32_t kValueFormatString = 4451;
constexpr int32_t kGameOptionsTitleString = 383;
// The main pause page's "Game Options" line, whose centred style the category lines copy.
constexpr int32_t kGameOptionsLineString = 380;

// Game item ids are hashes below 2^28.
constexpr uint32_t kGameItemQuit = 262066692;
constexpr uint32_t kGameItemGameOptions = 64626099;
// Game Options' Brightness line, which opens the game's brightness preset and gamma bar; it lives on Video Options > Display instead.
constexpr uint32_t kGameItemBrightness = 7177265;
// The brightness page's bar line.
constexpr uint32_t kGameItemBrightnessBar = 118456050;

// Our item ids, which the game's own dispatch never uses.
constexpr uint32_t kItemVideoPage = 0x7A5E0000;
constexpr uint32_t kItemBrightness = 0x7A5E0011;
constexpr uint32_t kItemGammaBar = 0x7A5E0013;
constexpr uint32_t kItemBrightnessLabel = 0x7A5E0014;
constexpr uint32_t kItemGammaLabel = 0x7A5E0015;
constexpr uint32_t kItemBarSpacer = 0x7A5E0016;
constexpr uint32_t kItemHints = 0x7A5E0026;
constexpr uint32_t kItemAudioPage = 0x7A5E0100;
constexpr uint32_t kItemModsPage = 0x7A5E0200;
constexpr uint32_t kItemControlsPage = 0x7A5E0400;
constexpr uint32_t kItemQuitConfirm = 0x7A5E0A00;
constexpr uint32_t kItemRestartGame = 0x7A5E0A01;
constexpr uint32_t kItemQuitBack = 0x7A5E0A02;
constexpr uint32_t kItemQuitApplication = 0x7A5E0A03;
constexpr uint32_t kItemRestoreDefaults = 0x7A5E0601;
constexpr uint32_t kItemRestoreAllDefaults = 0x7A5E0602;
constexpr uint32_t kItemConfirmRestore = 0x7A5E0603;
constexpr uint32_t kItemCancelRestore = 0x7A5E0604;
constexpr uint32_t kItemResume = 0x7A5E0605;
constexpr uint32_t kItemJumpStart = 0x7A5E0606;
constexpr uint32_t kItemJumpCancel = 0x7A5E0607;
constexpr uint32_t kItemOptionsPage = 0x7A5E0608;
constexpr uint32_t kItemPlayCredits = 0x7A5E0609;
constexpr uint32_t kItemLevelSelectPage = 0x7A5E0700;
constexpr uint32_t kItemLevelChapter = 0x7A5E0710;
constexpr uint32_t kItemLevelTask = 0x7A5E0800;

// The display gamma is shown as a second copy of the game's 28-segment brightness bar, spanning 0.5 to 2.0.
constexpr int32_t kGammaBarSegments = 28;

// Options is the main menu's list of categories, and ConfirmReset asks before a Restore Defaults.
enum class Page { None, Video, Audio, Controls, Coop, Gameplay, Mods, Hud, Graphics, Effects, Materials, Camera, Bindings, PadBindings, GameBrightness, Options, ConfirmReset, LevelSelect, LevelChapter, ConfirmJump };

struct Option {
  uint32_t id;
  Page page;
  const char16_t* label;
  std::u16string (*value)();
  void (*step)(int direction, bool wrap);
  // Game Options settings show the game's own localised value string, and some only show when the game's page would.
  int32_t (*game_value)() = nullptr;
  bool (*shown)() = nullptr;
};

// Each category page holds the options themselves, and B walks back up one level.
struct PageInfo {
  Page page;
  Page parent;
  const char16_t* title;
  uint32_t item;
};

std::span<const Option> AllOptions();
const Option* FindOption(uint32_t id);
uint32_t VisibleOptionCount(Page page);

const PageInfo* FindPage(Page page);
const PageInfo* FindPageByItem(uint32_t item);
// Where `page` sits among its parent's categories, so returning to the parent puts the cursor back on it.
uint32_t CategoryIndex(Page page);
// Where a page's line sits on the Options page; Game Options follows the categories.
uint32_t OptionsCategoryIndex(Page page);

void AddPageTitle(PPCContext& ctx, uint8_t* base, uint32_t window, const std::u16string& title);
void AddCategoryLine(PPCContext& ctx, uint8_t* base, uint32_t window, uint32_t id, const std::u16string& label);
// The same page reset activateSimplePauseSettingsMenu performs, then our page's lines instead of the game's settings.
void ActivatePage(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, Page page);
// Changes one option and rebuilds its page, which is how the game itself refreshes a changed value.
void ChangeOption(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, const Option& option, int direction, bool wrap);
// The main menu's options object, which the game kills where the pause menu would return to its main page.
bool IsOptionsObject(uint8_t* base, uint32_t pause);

// The page our menu last built; None once the game builds one of its own.
extern Page g_currentPage;
// The player whose menu is open, whose Game Options settings the options read and change.
extern uint32_t g_menuPlayer;
// Set while the game's brightness page was opened from Display, so its B returns there; the index is where the Brightness line sat.
extern bool g_brightnessFromDisplay;
extern uint32_t g_brightnessIndex;
// Set by the Resume line and carried out by xfixgem_storybook_initSimplePauseMenuWidget, where the game closes the pause menu itself, rather than from inside the pause object's own tick.
extern bool g_resumeRequested;
// Set by the Credits line; the credits start once the pause menu has closed.
extern bool g_playCreditsRequested;
// Level Select and Credits only appear in a game's pause menu, not the main menu's options.
extern bool g_levelSelectAvailable;

bool HasDefaults(Page scope);
void AddConfirmResetRows(PPCContext& ctx, uint8_t* base, uint32_t window);
// Handles an A press on a Restore Defaults line or the question it asks; false when `id` is not one of them.
bool PressRestoreDefaultsItem(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t id, uint32_t index);
// Leaves the Restore Defaults question for the page it was asked from; the pause menu's main page is the game's own, so its B handler rebuilds it.
void ReturnFromReset(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window);

// Adds the rows of the Level Select pages; false for any other page.
bool AddLevelSelectRows(PPCContext& ctx, uint8_t* base, uint32_t window, Page page);
// Handles an A press on a Level Select line; false when `id` is not one of them.
bool PressLevelSelectItem(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t id);
// B on a Level Select page; false when `page` is not one of them.
bool BackFromLevelSelect(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, Page page);
// Starts the task Level Select picked, once the pause menu has closed.
void StartPendingLevelJump(PPCContext& ctx, uint8_t* base);

int32_t GammaBarStep();
void SetGammaBarStep(int32_t step);

void RestoreQuitWindow(uint8_t* base);

}  // namespace kameo::ui
