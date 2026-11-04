#include "ui/pause_menu/pause_card.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <kameorepowered_init.h>

#include "app/process.h"
#include "core/localization.h"
#include "core/settings.h"
#include "core/time.h"
#include "core/unlock_codes.h"
#include "gpu/textures/texture_pack.h"
#include "input/input.h"
#include "ui/hud/loading_overlay.h"
#include "ui/pause_menu/pause_menu.h"
#include "ui/pause_menu/pause_menu_internal.h"

REXCVAR_DEFINE_INT32(kameo_menu_style, 0, "Kameo", "The pause menu: 0 = the storybook's own, 1 = the parchment card.");

namespace {

using kameo::l10n::Tr;
using namespace kameo::ui;

// The cards made for this menu, res/ui/pause_card.png and options_card.png, in their own pixels: one frame, the inner panel
// within it, and where the game's logo goes over the gap left for it (tools/ui/unlogo_pause_card.py painted the logo out of
// the art they were made with). Options' card adds a ring each side of the logo for the triggers.
namespace card_layout {
constexpr const char* kFiles[2] = {"pause_card.png", "options_card.png"};
constexpr uint32_t kWidth = 912, kHeight = 921;
constexpr float kFrameL = 41, kFrameT = 30, kFrameR = 871, kFrameB = 872;
constexpr float kPanelL = 116, kPanelT = 164, kPanelR = 794, kPanelB = 757;
constexpr float kLogoX = 262, kLogoY = -56, kLogoSize = 371, kLogoBottom = 205;
constexpr float kRingLX = 222, kRingRX = 684, kRingY = 108, kRingFace = 42;
}  // namespace card_layout

// Content hashes of the game's own textures (the language level's, loaded at boot), as dumps and replacements name them.
namespace art {
constexpr uint64_t kCard = 0xD7A07C4D4FEBBA2B;     // the pause menu's parchment card, curled corners and all
constexpr uint64_t kLogo = 0xBB2D9C9565BFB7C4;     // Kameo: Elements of Power
constexpr uint64_t kDiamond = 0xDE861A6D14133A8D;  // a small parchment diamond
constexpr uint64_t kTheena = 0xE49A61AAAADE82D3, kKalus = 0x806101AA8A9B8AA7, kTrainer = 0x6339480DDF15AE23, kFruit = 0xE4D8119C814139C9;
constexpr uint64_t kFlex = 0x8912D6B6BA688083, kThermite = 0x07C2B9ADE09A4B19;
constexpr uint64_t kButtonA = 0xEB240146A2D18131, kButtonB = 0x7C9738361C59FBC2, kButtonLT = 0xC975CD627DA1A29A, kButtonRT = 0xBA664054BBF19189;
}  // namespace art

// The storybook's own sounds: a page turning, its A press.
constexpr uint32_t kPageSound = 0xD0B7CAE;
constexpr uint32_t kSelectSound = 0xA4905F2;

// XInput's buttons, as the poll hands them to the game; the triggers are counted as two more.
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadStart = 0x0010, kPadLT = 0x0400,
                   kPadRT = 0x0800, kPadLB = 0x0100, kPadRB = 0x0200, kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;
constexpr int16_t kStickPush = 16000;
constexpr int64_t kRepeatDelayMs = 380;
constexpr int64_t kRepeatEveryMs = 95;
constexpr int64_t kOpenMs = 280;

std::string Utf8(std::u16string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    uint32_t c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size()) c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// ------------------------------------------------------------------------------------------------- state
enum class Sheet { Main, Options, Quit, Achievements };
enum class Action { Resume, ToOptions, Delegate, ToQuit, ToAchievements };
struct Line {
  std::string label;
  Action action;
  Page page = Page::None;  // the storybook page Delegate opens
  uint64_t face = 0;
};
struct Row {
  uint32_t id = 0;
  std::string label, value;
  bool binding = false;
};
struct Category {
  std::string title;
  std::vector<Row> rows;  // the category's own options, then its sub-pages'
};
struct Trophy {
  uint32_t id = 0;
  std::string label, description, score;
  bool unlocked = false;
};
// The quit choices, as the storybook's quit prompt offers them with keyboard and mouse.
enum QuitChoice { kQuitToMenu, kQuitApplication, kRestartGame, kBackToGame, kQuitChoices };

enum class RequestKind { Hover, Activate, Step, Back, Tab, Scroll };
struct Request {
  RequestKind kind;
  int a = 0, b = 0;
  Sheet sheet = Sheet::Main;  // the sheet the pointer was over; a request meant for another is dropped
};

struct State {
  std::mutex mutex;
  bool open = false;
  bool away = false;  // a storybook page the card opened is showing; the card comes back when it returns to the main page
  Page delegated = Page::None;  // that page
  int64_t opened_ms = 0;
  Sheet sheet = Sheet::Main;
  int line = 0;  // the main page's chosen line
  int tab = 0;
  int row = 0;
  int quit = kBackToGame;
  int trophy = 0;
  uint32_t changed_id = 0;  // the last change, for the value's flourish
  int64_t changed_ms = 0;
  std::vector<Line> lines;
  std::vector<Category> categories;
  std::vector<std::string> quit_lines;
  std::vector<Trophy> trophies;
  std::string trophy_summary;
  std::vector<Request> requests;
  uint32_t pause = 0, window = 0, move_sound = 0;
} g;

// Whether the card is up, for the draw hooks and the input paths that only need to know that, without the lock.
std::atomic<bool> g_open{false};

void SetOpen(bool open) {
  g.open = open;
  g_open.store(open, std::memory_order_relaxed);
}

struct PadMemory {
  uint16_t buttons = 0;
  int vertical = 0, horizontal = 0;
  int64_t repeat_at = 0;
};
PadMemory g_pads[2];
uint32_t g_polls = 0;

void PlaySound(PPCContext& ctx, uint8_t* base, uint32_t sound) {
  if (sound) CallGuest(ctx, base, sub_822EA728, sound);
}

// The storybook's main pause page, as B from its Options list returns to it.
void ReturnToMainPage(PPCContext& ctx, uint8_t* base) {
  if (g_currentPage == Page::None || !g.pause || !g.window) return;
  g_currentPage = Page::Options;
  PPCContext call = ctx;
  call.r3.u64 = g.window;
  call.r4.u64 = 0;
  call.r5.u64 = g.pause;
  sub_823F1028(call, base);
}

// The line of the storybook's main page Quit is on, or -1: only the main page lists it, which tells it from the quit
// prompt and the game's own pages whatever input mode laid them out.
int QuitLine(uint8_t* base, uint32_t window) {
  for (uint32_t i = 0; window && i < 32; ++i) {
    const uint32_t id = SelectedItemId(base, window, i);
    if (id == kGameItemQuit) return int(i);
  }
  return -1;
}

// Whether `page` is the one the card stepped aside for, or a page reached from it.
bool InDelegated(Page delegated, Page page) {
  if (page == delegated) return true;
  if (delegated == Page::LevelSelect) return page == Page::LevelChapter || page == Page::ConfirmJump;
  return false;
}

// What the card shows, as it is now. Game thread, lock held.
void Build(PPCContext& ctx, uint8_t* base) {
  g.lines.clear();
  g.lines.push_back({Utf8(Tr(u"Resume")), Action::Resume, Page::None, art::kTheena});
  g.lines.push_back({Utf8(Tr(u"Options")), Action::ToOptions, Page::None, art::kFlex});
  if (g_levelSelectAvailable && KameoCodeUnlocked(KameoUnlockCode::LevelSelect)) g.lines.push_back({Utf8(Tr(u"Level Select")), Action::Delegate, Page::LevelSelect, art::kKalus});
  if (g_levelSelectAvailable && KameoCodeUnlocked(KameoUnlockCode::CollectibleTracker)) g.lines.push_back({Utf8(Tr(u"Collectibles")), Action::Delegate, Page::Collectibles, art::kFruit});
  g.lines.push_back({Utf8(Tr(u"Achievements")), Action::ToAchievements, Page::None, art::kTrainer});
  g.lines.push_back({Utf8(Tr(u"Quit")), Action::ToQuit, Page::None, art::kThermite});
  g.quit_lines = {Utf8(Tr(u"Quit to Menu")), Utf8(Tr(u"Quit Application")), Utf8(Tr(u"Restart Game")), Utf8(Tr(u"Back to Game"))};
  g.categories.clear();
  for (const Page page : OptionsCategories()) {
    const PageInfo* info = FindPage(page);
    if (!info) continue;
    Category category{Utf8(Tr(info->title)), {}};
    const auto add = [&](Page from) {
      for (const Option& option : AllOptions()) {
        if (option.page != from || (option.shown && !option.shown())) continue;
        Row row{option.id, Utf8(Tr(option.label)), {}, from == Page::Bindings || from == Page::PadBindings};
        if (option.game_value) row.value = Utf8(PlainGuestText(base, LocalisedString(ctx, base, option.game_value())));
        else if (option.value) row.value = Utf8(option.value());
        category.rows.push_back(std::move(row));
      }
    };
    add(page);
    for (const PageInfo& child : AllPages()) {
      if (child.parent == page) add(child.page);
    }
    if (!category.rows.empty()) g.categories.push_back(std::move(category));
  }
  g.line = std::clamp(g.line, 0, std::max(0, int(g.lines.size()) - 1));
  g.tab = std::clamp(g.tab, 0, std::max(0, int(g.categories.size()) - 1));
}

// The title's achievements, read when the page opens.
void BuildTrophies() {
  g.trophies.clear();
  rex::system::KernelState* kernel = rex::system::kernel_state();
  const std::vector<rex::system::AchievementInfo> achievements = kernel ? kernel->achievements().ListAchievements() : std::vector<rex::system::AchievementInfo>{};
  if (achievements.empty()) {
    g.trophy_summary = Utf8(Tr(u"No achievement data was found in the game's files."));
    return;
  }
  uint32_t unlocked = 0, score = 0, total_score = 0;
  for (const auto& a : achievements) {
    const bool is_unlocked = kernel->achievements().IsUnlocked(a.id);
    total_score += a.gamerscore;
    if (is_unlocked) {
      ++unlocked;
      score += a.gamerscore;
    }
    const std::string& description = is_unlocked || a.unachieved_description.empty() ? a.description : a.unachieved_description;
    g.trophies.push_back({a.id, a.label, description, Utf8(Number(int32_t(a.gamerscore), u"G")), is_unlocked});
  }
  g.trophy_summary = Utf8(kameo::l10n::TrFormat(u"{0} of {1} unlocked   {2}G of {3}G", {Number(int32_t(unlocked), u""), Number(int32_t(achievements.size()), u""), Number(int32_t(score), u""), Number(int32_t(total_score), u"")}));
}

// The chosen line of the sheet showing, and how many it has.
int& Cursor() {
  switch (g.sheet) {
    case Sheet::Main: return g.line;
    case Sheet::Options: return g.row;
    case Sheet::Quit: return g.quit;
    case Sheet::Achievements: break;
  }
  return g.trophy;
}
int CursorCount() {
  switch (g.sheet) {
    case Sheet::Main: return int(g.lines.size());
    case Sheet::Options: return g.tab < int(g.categories.size()) ? int(g.categories[g.tab].rows.size()) : 0;
    case Sheet::Quit: return int(g.quit_lines.size());
    case Sheet::Achievements: break;
  }
  return int(g.trophies.size());
}

void Close() { SetOpen(false); }

void MoveCursor(PPCContext& ctx, uint8_t* base, int to) {
  to = std::clamp(to, 0, std::max(0, CursorCount() - 1));
  int& cursor = Cursor();
  if (to == cursor) return;
  cursor = to;
  PlaySound(ctx, base, g.move_sound);
}

void ShowSheet(PPCContext& ctx, uint8_t* base, Sheet sheet) {
  g.sheet = sheet;
  if (sheet == Sheet::Options) {
    g.tab = 0;
    g.row = 0;
  } else if (sheet == Sheet::Quit) {
    // The cursor starts on Back to Game, as the storybook's prompt starts on Back, so a stray press never quits.
    g.quit = kBackToGame;
  } else if (sheet == Sheet::Achievements) {
    g.trophy = 0;
    BuildTrophies();
  }
  PlaySound(ctx, base, kPageSound);
}

// The Options tabs, round in both directions.
void MoveTab(PPCContext& ctx, uint8_t* base, int to) {
  const int count = int(g.categories.size());
  if (!count) return;
  to = (to % count + count) % count;
  if (to == g.tab) return;
  g.tab = to;
  g.row = 0;
  PlaySound(ctx, base, kPageSound);
}

void StepOption(PPCContext& ctx, uint8_t* base, int direction, bool wrap) {
  if (g.sheet != Sheet::Options || g.row >= CursorCount()) return;
  const Row& row = g.categories[g.tab].rows[g.row];
  if (row.binding && !wrap) return;
  const Option* option = FindOption(row.id);
  if (!option) return;
  ApplyOption(ctx, base, g.pause, *option, direction, wrap);
  g.changed_id = row.id;
  g.changed_ms = KameoSteadyMilliseconds();
  PlaySound(ctx, base, kSelectSound);
  Build(ctx, base);
}

// The card steps aside for a storybook page, and comes back when that page returns to the main one.
void Delegate(PPCContext& ctx, uint8_t* base, Page page) {
  if (!g.pause || !g.window) return;
  Close();
  g.away = true;
  g.delegated = page;
  PlaySound(ctx, base, kSelectSound);
  ActivatePage(ctx, base, g.pause, g.window, page);
}

void Resume() {
  Close();
  g_resumeRequested = true;
}

void Choose(PPCContext& ctx, uint8_t* base, int choice) {
  switch (choice) {
    case kQuitToMenu: {
      // The storybook prompt's own Quit (its A, or its Quit to Menu line), which only sets the game's flags for leaving the
      // level; the card stays up until the pause ends with it.
      if (!g.window) break;
      const uint32_t menu = MenuOf(base, TextObjectOf(base, g.window));
      PlayMenuSound(ctx, base, REX_LOAD_U32(menu + kMenuSelectSound));
      CallMenuCallback(ctx, base, kReturnToLobbyCallback, g.window, 0, REX_LOAD_U32(menu + kMenuCallbackData));
      break;
    }
    case kQuitApplication:
      KameoSaveSettings();
      KameoQuitApplication();
      break;
    case kRestartGame:
      KameoSaveSettings();
      KameoRestartApplication();
      break;
    case kBackToGame:
      Resume();
      break;
  }
}

void Activate(PPCContext& ctx, uint8_t* base) {
  switch (g.sheet) {
    case Sheet::Options: StepOption(ctx, base, 1, true); return;
    case Sheet::Quit: Choose(ctx, base, g.quit); return;
    case Sheet::Achievements: return;
    case Sheet::Main: break;
  }
  if (g.line >= int(g.lines.size())) return;
  const Line& line = g.lines[g.line];
  switch (line.action) {
    case Action::Resume: Resume(); break;
    case Action::ToOptions: ShowSheet(ctx, base, Sheet::Options); break;
    case Action::ToQuit: ShowSheet(ctx, base, Sheet::Quit); break;
    case Action::ToAchievements: ShowSheet(ctx, base, Sheet::Achievements); break;
    case Action::Delegate: Delegate(ctx, base, line.page); break;
  }
}

void Back(PPCContext& ctx, uint8_t* base) {
  if (g.sheet != Sheet::Main) {
    g.sheet = Sheet::Main;
    PlaySound(ctx, base, kPageSound);
    return;
  }
  Resume();
}

void Open(PPCContext& ctx, uint8_t* base) {
  g.line = 0;
  g.tab = 0;
  g.row = 0;
  g.sheet = Sheet::Main;
  Build(ctx, base);
  SetOpen(true);
  g.away = false;
  g.opened_ms = KameoSteadyMilliseconds();
  g.requests.clear();
  // The press that paused is still held; it must not act as soon as the card opens.
  for (PadMemory& pad : g_pads) pad = PadMemory{0xFFFF};
  REXLOG_INFO("[kameo] pause card opened");
}

}  // namespace

void KameoKeepPauseCardArt() {
  using namespace art;
  kameo::gpu::KeepTextureCopies({kCard, kLogo, kDiamond, kTheena, kKalus, kTrainer, kFruit, kFlex, kThermite, kButtonA, kButtonB, kButtonLT, kButtonRT});
}

bool KameoPauseCardStyle() { return REXCVAR_GET(kameo_menu_style) == 1; }

bool KameoPauseCardOpen() { return g_open.load(std::memory_order_relaxed); }

void KameoPauseCardTick(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t page_id) {
  std::lock_guard lock(g.mutex);
  g.pause = pause;
  g.window = window;
  if (!pause || !window || !KameoPauseCardStyle() || IsOptionsObject(base, pause)) {
    if (g.open) Close();
    g.away = false;
    return;
  }
  // The main pause page: none of ours, not one of the game's settings pages, not the quit prompt.
  const bool main_page = g_currentPage == Page::None && page_id != 1 && page_id != 2 && QuitLine(base, window) >= 0;
  // A page the card opened was left for one it did not open (Level Select's B goes on to Gameplay): the card takes over again
  // over the main page.
  const bool left_delegated = g.away && g.delegated != Page::None && g_currentPage != Page::None && !InDelegated(g.delegated, g_currentPage);
  if (left_delegated) ReturnToMainPage(ctx, base);
  // Resume closes the pause menu on its next tick; the card must not open over it on the way out.
  if (g.open || (!main_page && !left_delegated) || g_resumeRequested) return;
  g.move_sound = REX_LOAD_U32(MenuOf(base, TextObjectOf(base, window)) + 120);
  if (g.away) {
    // Back from a page the card opened: the main page, on the line the player chose.
    Build(ctx, base);
    g.sheet = Sheet::Main;
    SetOpen(true);
    g.away = false;
    for (PadMemory& pad : g_pads) pad.buttons = 0xFFFF;
  } else {
    Open(ctx, base);
  }
}

bool KameoPauseCardTakePad(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state) {
  std::lock_guard lock(g.mutex);
  if (!g.open) return false;
  const int64_t now = KameoSteadyMilliseconds();
  // A pause that ended some other way (a level change, the Guide's quit, Quit to Menu) takes the card with it.
  if (KameoPausedBy() < 0 && now - g.opened_ms > 1500) {
    Close();
    return false;
  }
  if (user == 0) {
    std::vector<Request> requests;
    requests.swap(g.requests);
    for (const Request& r : requests) {
      if (!g.open) break;
      if (r.sheet != g.sheet) continue;
      switch (r.kind) {
        case RequestKind::Hover: MoveCursor(ctx, base, r.a); break;
        case RequestKind::Activate:
          Cursor() = std::clamp(r.a, 0, std::max(0, CursorCount() - 1));
          Activate(ctx, base);
          break;
        case RequestKind::Step:
          g.row = r.a;
          StepOption(ctx, base, r.b, false);
          break;
        case RequestKind::Back: Back(ctx, base); break;
        case RequestKind::Tab: MoveTab(ctx, base, r.a); break;
        case RequestKind::Scroll: MoveCursor(ctx, base, Cursor() + r.a); break;
      }
    }
    // A key or button binding finishes later, so the values are read again now and then.
    if (++g_polls % 20 == 0 && g.open) Build(ctx, base);
  }
  if (user < 2 && g.open) {
    PadMemory& pad = g_pads[user];
    const uint16_t buttons = REX_LOAD_U16(state + 4);
    const int16_t lx = int16_t(REX_LOAD_U16(state + 8));
    const int16_t ly = int16_t(REX_LOAD_U16(state + 10));
    const uint8_t lt = REX_LOAD_U8(state + 6), rt = REX_LOAD_U8(state + 7);
    const uint16_t held = buttons | (lt > 128 ? kPadLT : 0) | (rt > 128 ? kPadRT : 0);
    const uint16_t pressed = held & ~pad.buttons;
    pad.buttons = held;
    const int vertical = (buttons & kPadUp) || ly > kStickPush ? -1 : (buttons & kPadDown) || ly < -kStickPush ? 1 : 0;
    const int horizontal = (buttons & kPadLeft) || lx < -kStickPush ? -1 : (buttons & kPadRight) || lx > kStickPush ? 1 : 0;
    // A fresh direction acts at once; held, it repeats.
    const auto fire = [&](int direction, int& last) {
      if (direction == 0) {
        last = 0;
        return 0;
      }
      if (direction != last) {
        last = direction;
        pad.repeat_at = now + kRepeatDelayMs;
        return direction;
      }
      if (now < pad.repeat_at) return 0;
      pad.repeat_at = now + kRepeatEveryMs;
      return direction;
    };
    const bool options = g.sheet == Sheet::Options;
    if (const int v = fire(vertical, pad.vertical)) MoveCursor(ctx, base, Cursor() + v);
    if (const int h = fire(horizontal, pad.horizontal); h && options) StepOption(ctx, base, h, false);
    if (options && (pressed & (kPadLB | kPadLT | kPadX))) MoveTab(ctx, base, g.tab - 1);
    if (options && (pressed & (kPadRB | kPadRT | kPadY))) MoveTab(ctx, base, g.tab + 1);
    if (pressed & kPadA) Activate(ctx, base);
    if (pressed & kPadB) Back(ctx, base);
    if (pressed & kPadStart) Resume();
  }
  // The game sees a pad at rest; the packet number changes so it notices the release.
  std::memset(base + state + 4, 0, 12);
  REX_STORE_U32(state, REX_LOAD_U32(state) ^ 0x80000000u);
  return true;
}

// ------------------------------------------------------------------------------------------------- drawing

namespace {

// Ink on the parchment: near-black brown for what is chosen and for headings, a lighter brown for the rest, gold for the
// flourishes. The chosen line sits in a band of warm light.
constexpr ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kInk(0.15f, 0.08f, 0.03f, 1.0f);
constexpr ImVec4 kInkSoft(0.31f, 0.19f, 0.09f, 1.0f);
constexpr ImVec4 kInkGold(0.60f, 0.35f, 0.03f, 1.0f);
constexpr ImVec4 kWarm(1.0f, 0.88f, 0.84f, 1.0f);
// The parchment's own light, a hair below the ink, so the letters read as pressed into it.
constexpr ImVec4 kPressed(1.0f, 0.97f, 0.86f, 1.0f);
// On Options' panel: cream letters edged in deep violet, and a brighter gold.
constexpr ImVec4 kCream(1.0f, 0.96f, 0.86f, 1.0f);
constexpr ImVec4 kCreamSoft(0.88f, 0.82f, 0.86f, 1.0f);
constexpr ImVec4 kGoldBright(1.0f, 0.80f, 0.36f, 1.0f);
constexpr ImVec4 kShade(0.09f, 0.02f, 0.14f, 1.0f);

ImU32 Colour(const ImVec4& c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
float Ease(float t) { return 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f); }
float Approach(float current, float target, float dt, float speed) { return current < -0.5f ? target : current + (target - current) * (1.0f - std::exp(-speed * dt)); }
std::string Upper(const std::string& text) {
  std::string out;
  for (char c : text) out += char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// The parchment as the game lights it on its own pause card, rose-brown: each channel scaled and lifted, measured against
// the game's card over the same texture.
void LightParchment(std::vector<uint8_t>& rgba) {
  constexpr float kScale[3] = {0.67f, 0.55f, 0.80f};
  constexpr float kLift[3] = {28.0f, 25.0f, 26.0f};
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    for (int c = 0; c < 3; ++c) rgba[i + c] = uint8_t(std::clamp(float(rgba[i + c]) * kScale[c] + kLift[c], 0.0f, 255.0f));
  }
}

float Width(ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x; }

// `text` cut short with an ellipsis where it would run past `width`.
std::string Fit(ImFont* font, float size, std::string text, float width) {
  if (Width(font, size, text) <= width) return text;
  while (!text.empty() && Width(font, size, text + "...") > width) {
    size_t cut = text.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text + "...";
}

// Ink letters pressed into the parchment, or light ones edged in shade; `at` is the line's top left.
void Letter(ImDrawList* draw, ImFont* font, float size, ImVec2 at, const std::string& text, const ImVec4& ink, float alpha) {
  if (text.empty() || alpha <= 0.0f) return;
  if (ink.x * 0.3f + ink.y * 0.59f + ink.z * 0.11f > 0.5f) {
    const float edge = std::max(1.0f, size * 0.04f);
    for (const ImVec2 d : {ImVec2(-edge, 0), ImVec2(edge, 0), ImVec2(0, -edge), ImVec2(0, edge), ImVec2(edge, edge * 1.5f)}) {
      draw->AddText(font, size, ImVec2(at.x + d.x, at.y + d.y), Colour(kShade, 0.5f * alpha), text.c_str());
    }
  } else {
    const float press = std::max(1.0f, size * 0.03f);
    draw->AddText(font, size, ImVec2(at.x, at.y + press), Colour(kPressed, 0.5f * alpha), text.c_str());
  }
  draw->AddText(font, size, at, Colour(ink, alpha), text.c_str());
}
void LetterCentred(ImDrawList* draw, ImFont* font, float size, float centre_x, float y, const std::string& text, const ImVec4& ink, float alpha) {
  Letter(draw, font, size, ImVec2(centre_x - Width(font, size, text) * 0.5f, y), text, ink, alpha);
}

// Engraved capitals: the letters spread apart, as the game's ·KAMEO· headers are set.
float SpacedWidth(ImFont* font, float size, const std::string& text, float spacing) {
  float width = 0.0f;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    width += Width(font, size, text.substr(i, n)) + spacing;
    i += n;
  }
  return std::max(0.0f, width - spacing);
}
void Spaced(ImDrawList* draw, ImFont* font, float size, float centre_x, float y, const std::string& text, float spacing, const ImVec4& ink, float alpha) {
  float x = centre_x - SpacedWidth(font, size, text, spacing) * 0.5f;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    const std::string glyph = text.substr(i, n);
    Letter(draw, font, size, ImVec2(x, y), glyph, ink, alpha);
    x += Width(font, size, glyph) + spacing;
    i += n;
  }
}

// A thin ink line fading at both ends, with the parchment diamond in its middle.
void Rule(ImDrawList* draw, rex::ui::ImmediateTexture* diamond, float left, float right, float y, float s, float alpha) {
  const float mid = (left + right) * 0.5f;
  const float gap = 14 * s;
  draw->AddRectFilledMultiColor(ImVec2(left, y - 0.8f * s), ImVec2(mid - gap, y + 0.8f * s), Colour(kInkSoft, 0.0f), Colour(kInkSoft, 0.8f * alpha), Colour(kInkSoft, 0.8f * alpha), Colour(kInkSoft, 0.0f));
  draw->AddRectFilledMultiColor(ImVec2(mid + gap, y - 0.8f * s), ImVec2(right, y + 0.8f * s), Colour(kInkSoft, 0.8f * alpha), Colour(kInkSoft, 0.0f), Colour(kInkSoft, 0.0f), Colour(kInkSoft, 0.8f * alpha));
  if (diamond) draw->AddImage(reinterpret_cast<ImTextureID>(diamond), ImVec2(mid - 10 * s, y - 10 * s), ImVec2(mid + 10 * s, y + 10 * s), ImVec2(0, 0), ImVec2(1, 1), Colour(kWhite, alpha));
  else draw->AddQuadFilled(ImVec2(mid, y - 6 * s), ImVec2(mid + 6 * s, y), ImVec2(mid, y + 6 * s), ImVec2(mid - 6 * s, y), Colour(kInkSoft, alpha));
}

}  // namespace

rex::ui::ImmediateTexture* KameoPauseCard::Art(uint64_t hash) {
  if (const auto found = art_.find(hash); found != art_.end()) return found->second.get();
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  if (!immediate_drawer_ || !kameo::gpu::KeptTexture(hash, &width, &height, &rgba)) return nullptr;
  if (hash == art::kCard || hash == art::kDiamond) LightParchment(rgba);
  auto texture = immediate_drawer_->CreateTexture(width, height, rex::ui::ImmediateTextureFilter{}, false, rgba.data());
  if (!texture) return nullptr;
  return (art_[hash] = std::move(texture)).get();
}

rex::ui::ImmediateTexture* KameoPauseCard::OwnCard(int which) {
  if (own_card_tried_[which]) return own_cards_[which].get();
  if (!immediate_drawer_) return nullptr;
  own_card_tried_[which] = true;
  const std::filesystem::path path = kameo::l10n::ProgramFolder() / "ui" / card_layout::kFiles[which];
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  if (!kameo::gpu::LoadImageFile(path, &width, &height, &rgba) || width != card_layout::kWidth || height != card_layout::kHeight) {
    REXLOG_WARN("[kameo] pause card: {} is missing or not {}x{}, so that page uses the parchment card", path.string(), card_layout::kWidth, card_layout::kHeight);
    return nullptr;
  }
  own_cards_[which] = immediate_drawer_->CreateTexture(width, height, rex::ui::ImmediateTextureFilter{}, false, rgba.data());
  return own_cards_[which].get();
}

rex::ui::ImmediateTexture* KameoPauseCard::AchievementIcon(uint32_t id) {
  rex::system::KernelState* kernel = rex::system::kernel_state();
  if (!kernel) return nullptr;
  if (!icons_) {
    if (!runtime_ || !immediate_drawer_) return nullptr;
    icons_ = std::make_unique<rex::ui::AchievementIconCache>(immediate_drawer_, runtime_);
  }
  const auto info = kernel->achievements().FindAchievement(id);
  return info ? icons_->GetIcon(*info) : nullptr;
}

void KameoPauseCard::OnDraw(ImGuiIO& io) {
  // A copy of what the card shows, so the lock is not held while drawing.
  int64_t opened_ms = 0, changed_ms = 0;
  Sheet sheet = Sheet::Main;
  int line = 0, tab = 0, row = 0, quit = 0, trophy = 0;
  uint32_t changed_id = 0;
  std::vector<Line> lines;
  std::vector<Category> categories;
  std::vector<std::string> quit_lines;
  std::vector<Trophy> trophies;
  std::string trophy_summary;
  {
    std::lock_guard lock(g.mutex);
    if (!g.open) {
      main_mark_ = option_mark_ = -1.0f;
      return;
    }
    opened_ms = g.opened_ms;
    changed_ms = g.changed_ms;
    changed_id = g.changed_id;
    sheet = g.sheet;
    line = g.line;
    tab = g.tab;
    row = g.row;
    quit = g.quit;
    trophy = g.trophy;
    lines = g.lines;
    categories = g.categories;
    quit_lines = g.quit_lines;
    if (sheet == Sheet::Achievements) {
      trophies = g.trophies;
      trophy_summary = g.trophy_summary;
    }
  }
  const auto post = [sheet](RequestKind kind, int a = 0, int b = 0) {
    std::lock_guard lock(g.mutex);
    g.requests.push_back({kind, a, b, sheet});
  };
  const ImVec2 screen = io.DisplaySize;
  if (screen.x < 64 || screen.y < 64) return;
  const int64_t now = KameoSteadyMilliseconds();
  const float t = Ease(float(now - opened_ms) / float(kOpenMs));
  const float time = float(ImGui::GetTime());
  const float dt = std::min(io.DeltaTime, 0.1f);
  if (showed_sheet_ != int(sheet)) {
    showed_sheet_ = int(sheet);
    main_mark_ = option_mark_ = -1.0f;
  }
  ImFont* caps = KameoBookCapsFont() ? KameoBookCapsFont() : ImGui::GetFont();
  ImFont* text = KameoBookTextFont() ? KameoBookTextFont() : ImGui::GetFont();
  ImFont* italic = KameoBookItalicFont() ? KameoBookItalicFont() : text;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(screen);
  ImGui::Begin("##kameo_pause_card", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool mouse_moved = io.MousePos.x != last_mouse_.x || io.MousePos.y != last_mouse_.y;
  last_mouse_ = io.MousePos;
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const ImU32 white = Colour(kWhite, t);

  // The paused game, darkened, and the card, a little wider than tall, rising into the middle of the screen.
  draw->AddRectFilled(ImVec2(0, 0), screen, Colour(ImVec4(0.02f, 0.01f, 0.04f, 1.0f), 0.62f * t));
  float ch = screen.y * 0.84f;
  if (ch * 1.277f > screen.x * 0.92f) ch = screen.x * 0.92f / 1.277f;
  float cw = ch * 1.277f;
  const float cs = ch / 907.0f;  // sizes are set for a card 907 pixels tall, as on a 1080-line screen
  // The main page and Options have cards of their own, nearly square, drawn a little taller; `px` is one of their images'
  // pixels on screen.
  rex::ui::ImmediateTexture* own_card = sheet == Sheet::Main ? OwnCard(0) : sheet == Sheet::Options ? OwnCard(1) : nullptr;
  float px = 0.0f;
  if (own_card) {
    using namespace card_layout;
    ch = std::min(screen.y * 0.88f, screen.x * 0.92f * (kFrameB - kFrameT) / (kFrameR - kFrameL));
    px = ch / (kFrameB - kFrameT);
    cw = (kFrameR - kFrameL) * px;
  }
  const ImVec2 cmin((screen.x - cw) * 0.5f, (screen.y - ch) * 0.5f + (1.0f - t) * 40 * cs);
  const ImVec2 cmax(cmin.x + cw, cmin.y + ch);
  const float centre_x = (cmin.x + cmax.x) * 0.5f;
  // Where the own card's image pixel (x, y) is on screen.
  const auto card_at = [&](float x, float y) { return ImVec2(cmin.x + (x - card_layout::kFrameL) * px, cmin.y + (y - card_layout::kFrameT) * px); };
  for (int ring = 6; ring > 0; --ring) draw->AddRectFilled(ImVec2(cmin.x - ring * 3 * cs, cmin.y + ring * 4 * cs), ImVec2(cmax.x + ring * 3 * cs, cmax.y + ring * 6 * cs), Colour(ImVec4(0, 0, 0, 1), 0.07f * t), 24 * cs);
  if (own_card) {
    draw->AddImage(reinterpret_cast<ImTextureID>(own_card), card_at(0, 0), card_at(float(card_layout::kWidth), float(card_layout::kHeight)), ImVec2(0, 0), ImVec2(1, 1), white);
  } else if (auto* card = Art(art::kCard)) {
    draw->AddImage(reinterpret_cast<ImTextureID>(card), cmin, cmax, ImVec2(0, 0), ImVec2(1, 1), white);
  } else {
    static bool warned = false;
    if (!warned) REXLOG_WARN("[kameo] pause card: the parchment texture {:016X} has not been kept, so the card is drawn plain", art::kCard);
    warned = true;
    draw->AddRectFilled(cmin, cmax, Colour(ImVec4(0.62f, 0.44f, 0.41f, 1), t), 12 * cs);
  }
  auto* diamond = Art(art::kDiamond);
  float inner_l = cmin.x + cw * 0.09f, inner_r = cmax.x - cw * 0.09f;
  float inner_top = cmin.y + ch * 0.098f, inner_bottom = cmax.y - ch * 0.072f;
  if (own_card) {
    using namespace card_layout;
    inner_l = card_at(kPanelL, 0).x;
    inner_r = card_at(kPanelR, 0).x;
    inner_top = card_at(0, kPanelT).y;
    inner_bottom = card_at(0, kPanelB).y - 14 * cs;  // the footer stands off the panel's lower edge
  }
  // Options' own card has a rose panel under leaves, where light letters read better than ink.
  const bool lit = own_card && sheet == Sheet::Options;
  const ImVec4 ink = lit ? kCream : kInk, ink_soft = lit ? kCreamSoft : kInkSoft, ink_gold = lit ? kGoldBright : kInkGold;
  const float glow = 0.55f + 0.1f * std::sin(time * 3.0f);
  // The chosen line: a band of warm light, or of shade behind light letters, with the parchment diamond at its start.
  const auto band = [&](float y0, float y1, float l, float r) {
    const ImVec4& tone = lit ? kShade : kWarm;
    const float strength = lit ? 0.75f * glow : glow;
    draw->AddRectFilledMultiColor(ImVec2(l, y0), ImVec2(r, y1), Colour(tone, strength * t), Colour(tone, 0.0f), Colour(tone, 0.0f), Colour(tone, strength * t));
    const float d = (y1 - y0) * 0.22f, cy = (y0 + y1) * 0.5f;
    if (diamond) draw->AddImage(reinterpret_cast<ImTextureID>(diamond), ImVec2(l + 8 * cs, cy - d), ImVec2(l + 8 * cs + 2 * d, cy + d), ImVec2(0, 0), ImVec2(1, 1), white);
  };
  // A sheet's heading in engraved capitals between dots, with a rule under it; returns where the rule is.
  const auto heading = [&](const std::string& title) {
    const float size = 48 * cs;
    Spaced(draw, caps, size, centre_x, inner_top - 6 * cs, "\xC2\xB7 " + Upper(title) + " \xC2\xB7", 10 * cs, kInk, t);
    const float rule_y = inner_top + size + 16 * cs;
    Rule(draw, diamond, inner_l, inner_r, rule_y, cs, t);
    return rule_y;
  };
  // A controller button's face, or while the game has not loaded that one, its name on a small dark key.
  const auto button = [&](uint64_t face, const char* name, ImVec2 p0, float size) {
    if (auto* f = Art(face)) {
      draw->AddImage(reinterpret_cast<ImTextureID>(f), p0, ImVec2(p0.x + size, p0.y + size), ImVec2(0, 0), ImVec2(1, 1), white);
      return;
    }
    const float inset = size * 0.12f;
    draw->AddRectFilled(ImVec2(p0.x + inset, p0.y + inset), ImVec2(p0.x + size - inset, p0.y + size - inset), Colour(ImVec4(0.08f, 0.05f, 0.03f, 1), 0.9f * t), size * 0.14f);
    draw->AddRect(ImVec2(p0.x + inset, p0.y + inset), ImVec2(p0.x + size - inset, p0.y + size - inset), Colour(kInkGold, t), size * 0.14f, 0, std::max(1.0f, size * 0.05f));
    const float letters = size * 0.42f;
    draw->AddText(caps, letters, ImVec2(p0.x + (size - Width(caps, letters, name)) * 0.5f, p0.y + (size - letters) * 0.5f), Colour(kWhite, t), name);
  };
  // The buttons along the foot of the card: the game's faces, or the keys in keyboard and mouse mode.
  struct Prompt {
    std::vector<std::pair<uint64_t, const char*>> faces;
    const char* key;
    std::string label;
  };
  const auto footer = [&](const std::vector<Prompt>& prompts) {
    const bool keyboard = KameoKeyboardMouseMode();
    const float icon = 40 * cs, size = 28 * cs;
    float total = 0.0f;
    for (const Prompt& p : prompts) total += (keyboard ? Width(caps, size, p.key) + 18 * cs : float(p.faces.size()) * (icon + 4 * cs)) + 8 * cs + Width(caps, size, p.label) + 36 * cs;
    float x = centre_x - total * 0.5f;
    const float y = inner_bottom - icon;
    const float text_y = y + (icon - size) * 0.5f;
    for (const Prompt& p : prompts) {
      if (keyboard) {
        const float k = Width(caps, size, p.key);
        draw->AddRect(ImVec2(x, y + 3 * cs), ImVec2(x + k + 18 * cs, y + icon - 3 * cs), Colour(ink_soft, 0.9f * t), 6 * cs, 0, 1.6f * cs);
        Letter(draw, caps, size, ImVec2(x + 9 * cs, text_y), p.key, ink, t);
        x += k + 18 * cs;
      } else {
        for (const auto& [face, name] : p.faces) {
          button(face, name, ImVec2(x, y), icon);
          x += icon + 4 * cs;
        }
      }
      x += 8 * cs;
      Letter(draw, caps, size, ImVec2(x, text_y), p.label, ink_soft, t);
      x += Width(caps, size, p.label) + 36 * cs;
    }
  };
  // Where the text of a line `size` tall sits for its letters to be centred on `cy`.
  const auto text_top = [](float cy, float size) { return cy - size * 0.56f; };
  const float wheel = io.MouseWheel;
  // A list of choices in spaced capitals, centred on the card, as the main page and Quit set theirs.
  const auto choices = [&](const std::vector<std::string>& labels, int chosen, float list_top, float pitch, float list_l, float list_r, const std::vector<uint64_t>& faces) {
    if (chosen >= 0 && chosen < int(labels.size())) {
      main_mark_ = Approach(main_mark_, list_top + chosen * pitch, dt, 20.0f);
      band(main_mark_, main_mark_ + pitch - 6 * cs, list_l, list_r);
    }
    const float shift = faces.empty() ? 0.0f : 20 * cs;
    for (int i = 0; i < int(labels.size()); ++i) {
      const float y = list_top + i * pitch;
      const float cy = y + (pitch - 6 * cs) * 0.5f;
      const bool on = i == chosen;
      if (i < int(faces.size())) {
        if (auto* face = Art(faces[i])) {
          const float fs = pitch * 0.86f;
          draw->AddImage(reinterpret_cast<ImTextureID>(face), ImVec2(list_l + 34 * cs, cy - fs * 0.5f), ImVec2(list_l + 34 * cs + fs, cy + fs * 0.5f), ImVec2(0, 0), ImVec2(1, 1), white);
        }
      }
      const float size = 44 * cs;
      Spaced(draw, caps, size, centre_x + shift, text_top(cy, size), Upper(labels[i]), 4 * cs, on ? kInk : kInkSoft, t);
      if (ImGui::IsMouseHoveringRect(ImVec2(list_l, y), ImVec2(list_r, y + pitch - 6 * cs))) {
        if (mouse_moved && !on) post(RequestKind::Hover, i);
        if (clicked) post(RequestKind::Activate, i);
      }
    }
  };

  if (sheet == Sheet::Main) {
    // The main page: the logo, then the short list, each line with a face beside it.
    std::vector<std::string> labels;
    std::vector<uint64_t> faces;
    for (const Line& l : lines) {
      labels.push_back(l.label);
      faces.push_back(l.face);
    }
    if (own_card) {
      // The logo over the gap the card leaves for it at the top of its panel, the list centred in the panel below it.
      using namespace card_layout;
      if (auto* logo = Art(art::kLogo)) draw->AddImage(reinterpret_cast<ImTextureID>(logo), card_at(kLogoX, kLogoY), card_at(kLogoX + kLogoSize, kLogoY + kLogoSize), ImVec2(0, 0), ImVec2(1, 1), white);
      const float list_top = card_at(0, kLogoBottom).y + 12 * cs, list_bottom = inner_bottom - 40 * cs - 24 * cs;
      const float pitch = std::min(ch * 0.066f, (list_bottom - list_top) / float(std::max<size_t>(1, lines.size())));
      choices(labels, line, list_top + (list_bottom - list_top - pitch * float(lines.size())) * 0.5f, pitch, centre_x - 300 * cs, centre_x + 300 * cs, faces);
    } else {
      if (auto* logo = Art(art::kLogo)) {
        const float lw = cw * 0.40f;
        draw->AddImage(reinterpret_cast<ImTextureID>(logo), ImVec2(centre_x - lw * 0.5f, inner_top - ch * 0.065f), ImVec2(centre_x + lw * 0.5f, inner_top - ch * 0.065f + lw), ImVec2(0, 0), ImVec2(1, 1), white);
      }
      const float pitch = ch * 0.066f;
      choices(labels, line, inner_bottom - ch * 0.075f - pitch * float(lines.size()), pitch, cmin.x + cw * 0.27f, cmax.x - cw * 0.27f, faces);
    }
    footer({{{{art::kButtonA, "A"}}, "Enter", Utf8(Tr(u"Select"))}, {{{art::kButtonB, "B"}}, "Esc", Utf8(Tr(u"Resume"))}});
  } else if (sheet == Sheet::Options) {
    // Options: the heading, the categories as tabs between the triggers that change them, the chosen one's settings below.
    // On its own card the logo heads it and the triggers sit in the rings beside the logo, leaving the tabs the panel's width.
    float rule_y = 0.0f;
    if (own_card) {
      using namespace card_layout;
      if (auto* logo = Art(art::kLogo)) draw->AddImage(reinterpret_cast<ImTextureID>(logo), card_at(kLogoX, kLogoY), card_at(kLogoX + kLogoSize, kLogoY + kLogoSize), ImVec2(0, 0), ImVec2(1, 1), white);
      rule_y = card_at(0, kLogoBottom).y - 6 * cs;
    } else {
      rule_y = heading(Utf8(Tr(u"Options")));
    }
    const int tab_count = int(categories.size());
    const float trigger = 44 * cs;
    const float tab_l = own_card ? inner_l + 16 * cs : inner_l + trigger + 16 * cs, tab_r = own_card ? inner_r - 16 * cs : inner_r - trigger - 16 * cs;
    // The categories are larger than the settings, and made smaller only when a language's names would not fit.
    float label_size = 44 * cs;
    const float min_gap = 24 * cs;
    float names = 0.0f;
    for (const Category& c : categories) names += Width(caps, label_size, c.title);
    if (tab_count && names + min_gap * float(tab_count) > tab_r - tab_l) {
      const float fit = (tab_r - tab_l - min_gap * float(tab_count)) / names;
      label_size *= fit;
      names *= fit;
    }
    const float gap = tab_count ? (tab_r - tab_l - names) / float(tab_count) : 0.0f;
    const float tabs_top = rule_y + 18 * cs;
    const float tabs_h = label_size + 14 * cs;
    const float trigger_y = tabs_top + label_size * 0.5f - trigger * 0.5f;
    if (own_card) {
      using namespace card_layout;
      const float face = kRingFace * px;
      button(art::kButtonLT, "LT", ImVec2(card_at(kRingLX, 0).x - face * 0.5f, card_at(0, kRingY).y - face * 0.5f), face);
      button(art::kButtonRT, "RT", ImVec2(card_at(kRingRX, 0).x - face * 0.5f, card_at(0, kRingY).y - face * 0.5f), face);
    } else {
      button(art::kButtonLT, "LT", ImVec2(inner_l, trigger_y), trigger);
      button(art::kButtonRT, "RT", ImVec2(inner_r - trigger, trigger_y), trigger);
    }
    float x = tab_l + gap * 0.5f;
    for (int i = 0; i < tab_count; ++i) {
      const bool on = i == tab;
      const float w = Width(caps, label_size, categories[i].title);
      Letter(draw, caps, label_size, ImVec2(x, tabs_top), categories[i].title, on ? ink : ink_soft, t);
      if (on) {
        // The chosen category underlined in gold, fading at both ends.
        const float uy = tabs_top + label_size + 4 * cs, ux0 = x - 6 * cs, ux1 = x + w + 6 * cs, mid = (ux0 + ux1) * 0.5f;
        draw->AddRectFilledMultiColor(ImVec2(ux0, uy), ImVec2(mid, uy + 3 * cs), Colour(ink_gold, 0.0f), Colour(ink_gold, t), Colour(ink_gold, t), Colour(ink_gold, 0.0f));
        draw->AddRectFilledMultiColor(ImVec2(mid, uy), ImVec2(ux1, uy + 3 * cs), Colour(ink_gold, t), Colour(ink_gold, 0.0f), Colour(ink_gold, 0.0f), Colour(ink_gold, t));
      }
      if (ImGui::IsMouseHoveringRect(ImVec2(x - gap * 0.5f, tabs_top - 8 * cs), ImVec2(x + w + gap * 0.5f, tabs_top + tabs_h)) && clicked) post(RequestKind::Tab, i);
      x += w + gap;
    }
    // The settings, as many as the card holds, scrolled to keep the chosen one in view.
    if (tab < tab_count) {
      const Category& category = categories[tab];
      const float rows_top = tabs_top + tabs_h + 14 * cs;
      const float pitch = 54 * cs;
      const float rows_bottom = inner_bottom - 58 * cs;
      const int fits = std::max(1, int((rows_bottom - rows_top) / pitch));
      const int first = std::clamp(row - fits + 2, 0, std::max(0, int(category.rows.size()) - fits));
      option_mark_ = Approach(option_mark_, rows_top + (row - first) * pitch, dt, 20.0f);
      band(option_mark_, option_mark_ + pitch - 6 * cs, inner_l, inner_r);
      for (int i = first; i < int(category.rows.size()) && i < first + fits; ++i) {
        const Row& r = category.rows[i];
        const float ry = rows_top + (i - first) * pitch;
        const float cy = ry + (pitch - 6 * cs) * 0.5f;
        const bool on = i == row;
        const float label_size_row = 40 * cs;
        Letter(draw, text, label_size_row, ImVec2(inner_l + 44 * cs, text_top(cy, label_size_row)), r.label, on ? ink : ink_soft, t);
        // The value in small capitals between gold arrows; a changed one flashes gold.
        const float flourish = changed_id == r.id ? std::clamp(1.0f - float(now - changed_ms) / 300.0f, 0.0f, 1.0f) : 0.0f;
        const ImVec4 value_colour = Mix(on ? ink : ink_soft, ink_gold, flourish);
        const float value_size = 34 * cs;
        const float v = Width(caps, value_size, r.value);
        const float vr = inner_r - 14 * cs, arrow = 22 * cs;
        const bool hovered = ImGui::IsMouseHoveringRect(ImVec2(inner_l, ry), ImVec2(inner_r, ry + pitch - 6 * cs));
        if (hovered && mouse_moved && !on) post(RequestKind::Hover, i);
        if (!r.binding) {
          const float a = on ? 1.0f : 0.4f;
          draw->AddTriangleFilled(ImVec2(vr, cy), ImVec2(vr - arrow * 0.6f, cy - 9 * cs), ImVec2(vr - arrow * 0.6f, cy + 9 * cs), Colour(ink_gold, a * t));
          const float lx = vr - arrow - v - arrow;
          draw->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + arrow * 0.6f, cy - 9 * cs), ImVec2(lx + arrow * 0.6f, cy + 9 * cs), Colour(ink_gold, a * t));
          Letter(draw, caps, value_size, ImVec2(vr - arrow - v, text_top(cy, value_size)), r.value, value_colour, t);
          if (hovered && clicked) {
            const bool back = ImGui::IsMouseHoveringRect(ImVec2(lx - 12 * cs, ry), ImVec2(lx + arrow + 6 * cs, ry + pitch));
            post(back ? RequestKind::Step : RequestKind::Activate, i, back ? -1 : 1);
          }
        } else {
          Letter(draw, caps, value_size, ImVec2(vr - v, text_top(cy, value_size)), r.value, value_colour, t);
          draw->AddRect(ImVec2(vr - v - 10 * cs, cy - 20 * cs), ImVec2(vr + 8 * cs, cy + 20 * cs), Colour(ink_soft, (on ? 0.9f : 0.4f) * t), 5 * cs, 0, 1.4f * cs);
          if (hovered && clicked) post(RequestKind::Activate, i);
        }
      }
    }
    if (wheel != 0.0f && ImGui::IsMouseHoveringRect(cmin, cmax)) post(RequestKind::Scroll, wheel > 0 ? -1 : 1);
    footer({{{{art::kButtonA, "A"}}, "Enter", Utf8(Tr(u"Select"))}, {{{art::kButtonB, "B"}}, "Backspace", Utf8(Tr(u"Back"))}, {{{art::kButtonLT, "LT"}, {art::kButtonRT, "RT"}}, "Q  E", Utf8(Tr(u"Category"))}});
  } else if (sheet == Sheet::Quit) {
    // Quit: the storybook prompt's choices, with its warning.
    const float rule_y = heading(Utf8(Tr(u"Quit")));
    float warn_size = 34 * cs;
    const std::string warning = Utf8(Tr(u"Any progress since you last saved will be lost."));
    const float warn_w = Width(italic, warn_size, warning);
    if (warn_w > inner_r - inner_l) warn_size *= (inner_r - inner_l) / warn_w;
    LetterCentred(draw, italic, warn_size, centre_x, rule_y + 26 * cs, warning, kInkSoft, t);
    const float pitch = 66 * cs;
    const float list_top = (rule_y + 26 * cs + warn_size + inner_bottom - 60 * cs) * 0.5f - pitch * float(quit_lines.size()) * 0.5f;
    choices(quit_lines, quit, list_top, pitch, cmin.x + cw * 0.25f, cmax.x - cw * 0.25f, {});
    footer({{{{art::kButtonA, "A"}}, "Enter", Utf8(Tr(u"Select"))}, {{{art::kButtonB, "B"}}, "Esc", Utf8(Tr(u"Back"))}});
  } else {
    // Achievements: the tally, then each with its art, name, description and gamerscore; a locked one's art is dimmed, as
    // the console showed it.
    const float rule_y = heading(Utf8(Tr(u"Achievements")));
    const float summary_size = 30 * cs;
    LetterCentred(draw, italic, summary_size, centre_x, rule_y + 14 * cs, trophy_summary, kInkSoft, t);
    const float rows_top = rule_y + 20 * cs + summary_size + 14 * cs;
    const float pitch = 88 * cs;
    const float rows_bottom = inner_bottom - 56 * cs;
    const int fits = std::max(1, int((rows_bottom - rows_top) / pitch));
    const int count = int(trophies.size());
    const int first = std::clamp(trophy - fits / 2, 0, std::max(0, count - fits));
    if (count) {
      option_mark_ = Approach(option_mark_, rows_top + (trophy - first) * pitch, dt, 20.0f);
      band(option_mark_, option_mark_ + pitch - 6 * cs, inner_l, inner_r);
    }
    draw->PushClipRect(ImVec2(inner_l, rows_top - 4 * cs), ImVec2(inner_r, rows_top + fits * pitch), true);
    for (int i = first; i < count && i < first + fits; ++i) {
      const Trophy& a = trophies[i];
      const float ry = rows_top + (i - first) * pitch;
      const bool on = i == trophy;
      const float icon = pitch - 22 * cs;
      const float icon_x = inner_l + 44 * cs, icon_y = ry + (pitch - 6 * cs - icon) * 0.5f;
      draw->AddRect(ImVec2(icon_x - 2 * cs, icon_y - 2 * cs), ImVec2(icon_x + icon + 2 * cs, icon_y + icon + 2 * cs), Colour(kInkSoft, 0.9f * t), 3 * cs, 0, 2 * cs);
      if (auto* art = AchievementIcon(a.id)) draw->AddImage(reinterpret_cast<ImTextureID>(art), ImVec2(icon_x, icon_y), ImVec2(icon_x + icon, icon_y + icon), ImVec2(0, 0), ImVec2(1, 1), a.unlocked ? white : Colour(ImVec4(0.45f, 0.42f, 0.38f, 1), 0.85f * t));
      const float score_size = 34 * cs;
      const float score_w = Width(caps, score_size, a.score);
      const float text_x = icon_x + icon + 20 * cs, text_r = inner_r - 24 * cs - score_w - 24 * cs;
      const float name_size = 36 * cs, description_size = 28 * cs;
      const float alpha = a.unlocked ? 1.0f : 0.7f;
      Letter(draw, text, name_size, ImVec2(text_x, ry + 8 * cs), Fit(text, name_size, a.label, text_r - text_x), on ? kInk : kInkSoft, alpha * t);
      Letter(draw, italic, description_size, ImVec2(text_x, ry + 10 * cs + name_size), Fit(italic, description_size, a.description, text_r - text_x), kInkSoft, alpha * t);
      Letter(draw, caps, score_size, ImVec2(inner_r - 24 * cs - score_w, text_top(ry + (pitch - 6 * cs) * 0.5f, score_size)), a.score, on ? kInk : kInkSoft, alpha * t);
      if (ImGui::IsMouseHoveringRect(ImVec2(inner_l, ry), ImVec2(inner_r, ry + pitch - 6 * cs)) && mouse_moved && !on) post(RequestKind::Hover, i);
    }
    draw->PopClipRect();
    // How far down the list is, as a thin bar at the card's right edge.
    if (count > fits) {
      const float track_top = rows_top, track_bottom = rows_top + fits * pitch, x = inner_r + 10 * cs;
      draw->AddRectFilled(ImVec2(x, track_top), ImVec2(x + 4 * cs, track_bottom), Colour(kInkSoft, 0.25f * t), 2 * cs);
      const float span = (track_bottom - track_top) * float(fits) / float(count);
      const float top = track_top + (track_bottom - track_top - span) * float(first) / float(count - fits);
      draw->AddRectFilled(ImVec2(x, top), ImVec2(x + 4 * cs, top + span), Colour(kInkGold, 0.9f * t), 2 * cs);
    }
    if (wheel != 0.0f && ImGui::IsMouseHoveringRect(cmin, cmax)) post(RequestKind::Scroll, wheel > 0 ? -1 : 1);
    footer({{{{art::kButtonB, "B"}}, "Esc", Utf8(Tr(u"Back"))}});
  }
  // A right click, as everywhere in the game's menus, goes back.
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) post(RequestKind::Back);
  ImGui::End();
}

// _textWindow_renderbuffer(window) -- while the card is up, the storybook's lines (and the rest of the game's text, behind the
// darkened screen) are not drawn, so only the card shows.
extern "C" REX_FUNC(sub_82108A70) {
  if (KameoPauseCardOpen()) return;
  __imp__sub_82108A70(ctx, base);
}

// _bookContextInfoCardObject_renderbuffer -- the strip under the storybook's pause card ("Press START to continue") is hidden
// with it.
extern "C" REX_FUNC(sub_823D00A0) {
  if (KameoPauseCardOpen()) return;
  __imp__sub_823D00A0(ctx, base);
}
