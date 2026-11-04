// Level Select: the story's tasks grouped by region, and starting one the way the storybook's task jump does.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include <rex/system/kernel_state.h>

#include "core/guest.h"
#include "core/localization.h"
#include "engine/current_level.h"
#include "engine/custom_levels.h"
#include "engine/dlc/dlc_swap.h"
#include "engine/guest_addresses.h"
#include "core/time.h"
#include "gpu/replay/screen_layout.h"
#include "engine/kept_progress.h"
#include "ui/pause_menu/pause_menu_internal.h"

namespace kameo::ui {

namespace {

// The 229-entry task table taskStartLevel reads, 48 bytes each: level name at +0, checkpoint name string at +40.
constexpr uint32_t kTaskTable = 0x8205DE7C;
constexpr uint32_t kTaskEntryBytes = 48;
constexpr uint32_t kTaskNameString = 40;

struct LevelChapter {
  const char16_t* title;
  uint16_t first_task;
  uint16_t last_task;
};

constexpr LevelChapter kLevelChapters[] = {
    {TR_NOOP(u"Thorn's Castle"), 1, 13},       {TR_NOOP(u"Enchanted Kingdom"), 14, 31}, {TR_NOOP(u"The Badlands"), 32, 36},     {TR_NOOP(u"Forgotten Forest"), 37, 64},
    {TR_NOOP(u"Forest Temple"), 65, 68},       {TR_NOOP(u"Mountain Falls"), 69, 84},    {TR_NOOP(u"Mountain Rise"), 85, 107},   {TR_NOOP(u"Water Temple"), 108, 125},
    {TR_NOOP(u"Snow Top Village"), 126, 152},  {TR_NOOP(u"Snow Temple"), 153, 168},     {TR_NOOP(u"Ogre's Swamp"), 169, 184},   {TR_NOOP(u"Thorn's Pass"), 185, 202},
    {TR_NOOP(u"Thorn's Airship"), 203, 225},
};

// After the story's chapters, the custom levels' chapter, shown when there are any.
constexpr uint32_t kCustomChapter = uint32_t(std::size(kLevelChapters));

uint32_t g_levelChapter = 0;
uint32_t g_levelPickedTask = 0;
// The custom level picked instead of a task, or -1.
int32_t g_levelPickedCustom = -1;
// A task to start once the pause menu has closed, or -1.
int32_t g_levelJumpTask = -1;
// A custom level to load once the pause menu has closed, or -1.
int32_t g_levelJumpCustom = -1;

// The task's checkpoint name as the game's save list shows it, without its {tags} or line breaks.
std::u16string TaskName(PPCContext& ctx, uint8_t* base, uint32_t task) {
  const uint32_t text = LocalisedString(ctx, base, int32_t(REX_LOAD_U32(kTaskTable + kTaskEntryBytes * task + kTaskNameString)));
  std::u16string name;
  bool in_tag = false;
  for (uint32_t i = 0; text && i < 256; ++i) {
    const char16_t c = char16_t(REX_LOAD_U16(text + i * 2));
    if (!c) break;
    if (c == u'{') in_tag = true;
    else if (c == u'}') in_tag = false;
    else if (!in_tag) name.push_back(c == u'\n' ? u' ' : c);
  }
  while (!name.empty() && name.front() == u' ') name.erase(name.begin());
  while (!name.empty() && name.back() == u' ') name.pop_back();
  return name.empty() ? Number(int32_t(task), u"") : name;
}

// Most task names end in their area's name, which the chapter page already shows as its title.
// The names are the game's, in its language, so the area is whichever " - " ending most of the chapter's tasks share.
std::vector<std::u16string> WithoutChapter(std::vector<std::u16string> names) {
  std::map<std::u16string, size_t> endings;
  for (const std::u16string& name : names) {
    if (const size_t dash = name.rfind(u" - "); dash != std::u16string::npos && dash > 0) ++endings[name.substr(dash)];
  }
  const auto common = std::max_element(endings.begin(), endings.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  if (common == endings.end() || common->second * 2 < names.size()) return names;
  for (std::u16string& name : names) {
    if (name.size() > common->first.size() && name.ends_with(common->first)) name.resize(name.size() - common->first.size());
  }
  return names;
}

uint32_t PickedTaskIndex() {
  if (g_levelPickedCustom >= 0) return uint32_t(g_levelPickedCustom);
  return g_levelPickedTask - kLevelChapters[g_levelChapter].first_task;
}

}  // namespace

bool AddLevelSelectRows(PPCContext& ctx, uint8_t* base, uint32_t window, Page page) {
  if (page == Page::LevelSelect) {
    AddPageTitle(ctx, base, window, kameo::l10n::Tr(u"Level Select"));
    for (uint32_t i = 0; i < std::size(kLevelChapters); ++i) AddCategoryLine(ctx, base, window, kItemLevelChapter + i, kameo::l10n::Tr(kLevelChapters[i].title));
    if (!engine::CustomLevels().empty()) AddCategoryLine(ctx, base, window, kItemLevelChapter + kCustomChapter, kameo::l10n::Tr(u"Custom Levels"));
    return true;
  }
  if (page == Page::LevelChapter && g_levelChapter == kCustomChapter) {
    AddPageTitle(ctx, base, window, kameo::l10n::Tr(u"Custom Levels"));
    const auto& levels = engine::CustomLevels();
    for (uint32_t i = 0; i < levels.size(); ++i) AddCategoryLine(ctx, base, window, kItemCustomLevel + i, Number(int32_t(i + 1), u". ") + levels[i].title);
    return true;
  }
  if (page == Page::LevelChapter) {
    const LevelChapter& chapter = kLevelChapters[g_levelChapter];
    AddPageTitle(ctx, base, window, kameo::l10n::Tr(chapter.title));
    std::vector<std::u16string> names;
    for (uint32_t task = chapter.first_task; task <= chapter.last_task; ++task) names.push_back(TaskName(ctx, base, task));
    names = WithoutChapter(std::move(names));
    for (uint32_t i = 0; i < names.size(); ++i) AddCategoryLine(ctx, base, window, kItemLevelTask + chapter.first_task + i, Number(int32_t(i + 1), u". ") + names[i]);
    return true;
  }
  if (page == Page::ConfirmJump && g_levelPickedCustom >= 0 && size_t(g_levelPickedCustom) < engine::CustomLevels().size()) {
    AddPageTitle(ctx, base, window, kameo::l10n::TrFormat(u"Start \"{0}\"?", {engine::CustomLevels()[g_levelPickedCustom].title}));
    if (const uint32_t note = GuestWideString(kameo::l10n::Tr(u"A custom level;\nthe story stays where it is."))) AddTextItem(ctx, base, window, 0, GuestWideString(u"{alignment='centre' size='21'}"), note, 0, false);
    AddCategoryLine(ctx, base, window, kItemJumpStart, kameo::l10n::Tr(u"Start"));
    AddCategoryLine(ctx, base, window, kItemJumpCancel, kameo::l10n::Tr(u"Cancel"));
    return true;
  }
  if (page == Page::ConfirmJump) {
    AddPageTitle(ctx, base, window, kameo::l10n::TrFormat(u"Start from \"{0}\"?", {TaskName(ctx, base, g_levelPickedTask)}));
    if (const uint32_t note = GuestWideString(kameo::l10n::Tr(u"The story is set to this point;\ncollectibles are kept."))) AddTextItem(ctx, base, window, 0, GuestWideString(u"{alignment='centre' size='21'}"), note, 0, false);
    AddCategoryLine(ctx, base, window, kItemJumpStart, kameo::l10n::Tr(u"Start"));
    AddCategoryLine(ctx, base, window, kItemJumpCancel, kameo::l10n::Tr(u"Cancel"));
    return true;
  }
  return false;
}

bool PressLevelSelectItem(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t id) {
  if (id == kItemLevelSelectPage) {
    ActivatePage(ctx, base, pause, window, Page::LevelSelect);
    return true;
  }
  if (id >= kItemLevelChapter && id <= kItemLevelChapter + kCustomChapter) {
    g_levelChapter = id - kItemLevelChapter;
    ActivatePage(ctx, base, pause, window, Page::LevelChapter);
    return true;
  }
  if (id > kItemLevelTask && id <= kItemLevelTask + kLevelChapters[std::size(kLevelChapters) - 1].last_task) {
    g_levelPickedTask = id - kItemLevelTask;
    g_levelPickedCustom = -1;
    ActivatePage(ctx, base, pause, window, Page::ConfirmJump);
    // The cursor starts on Cancel, so a stray second press does not replace anything.
    KeepCursor(ctx, base, window, 1);
    return true;
  }
  if (id >= kItemCustomLevel && id < kItemCustomLevel + engine::CustomLevels().size()) {
    g_levelPickedCustom = int32_t(id - kItemCustomLevel);
    ActivatePage(ctx, base, pause, window, Page::ConfirmJump);
    KeepCursor(ctx, base, window, 1);
    return true;
  }
  if (id == kItemJumpStart) {
    if (g_levelPickedCustom >= 0) g_levelJumpCustom = g_levelPickedCustom;
    else g_levelJumpTask = int32_t(g_levelPickedTask);
    g_resumeRequested = true;
    return true;
  }
  if (id == kItemJumpCancel) {
    ActivatePage(ctx, base, pause, window, Page::LevelChapter);
    KeepCursor(ctx, base, window, PickedTaskIndex());
    return true;
  }
  return false;
}

bool BackFromLevelSelect(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, Page page) {
  if (page == Page::ConfirmJump) {
    ActivatePage(ctx, base, pause, window, Page::LevelChapter);
    KeepCursor(ctx, base, window, PickedTaskIndex());
    return true;
  }
  if (page == Page::LevelChapter) {
    ActivatePage(ctx, base, pause, window, Page::LevelSelect);
    KeepCursor(ctx, base, window, g_levelChapter);
    return true;
  }
  if (page == Page::LevelSelect) {
    ActivatePage(ctx, base, pause, window, Page::Gameplay);
    KeepCursor(ctx, base, window, VisibleOptionCount(Page::Gameplay));
    return true;
  }
  return false;
}

void RequestCustomLevelJump(int32_t index) {
  g_levelJumpCustom = index;
  g_resumeRequested = true;
}

void JumpToTask(PPCContext& ctx, uint8_t* base, int32_t task) {
  g_levelJumpTask = task;
  StartPendingLevelJump(ctx, base);
}

void StartPendingLevelJump(PPCContext& ctx, uint8_t* base) {
  if (const int32_t custom = g_levelJumpCustom; custom >= 0) {
    g_levelJumpCustom = -1;
    if (size_t(custom) < engine::CustomLevels().size()) {
      const engine::CustomLevel& level = engine::CustomLevels()[custom];
      ChangeToLevel(ctx, base, level.name, level.start);
    }
    return;
  }
  const int32_t task = g_levelJumpTask;
  g_levelJumpTask = -1;
  if (task < 0) return;
  PPCContext net = ctx;
  sub_8229F978(net, base);
  if (net.r3.u32 != 0) return;
  // storyBookFrontEndStartTask, the storybook's task jump, for one player: the player's starting stats and per-player counters are reset, taskStartTaskNoLevelChange clears the story's events and warriors and sets every prerequisite task's done, and taskStartLevel changes to the task's level.
  for (uint32_t i = 0; 0x82733A5Cu + 8 * i < 0x82733A9Cu; ++i) REX_STORE_U32(0x82885148u + 4 * i, REX_LOAD_U32(0x82733A5Cu + 8 * i));
  constexpr uint32_t kClearedWords[] = {0x8289BA64, 0x8289AED0, 0x8289BA6C, 0x82884F38, 0x8289BA6E, 0x82884F3A, 0x8289BA70, 0x82884F3C, 0x8289BA72, 0x82884F3E, 0x82885140, 0x82885142,
                                        0x82884F10, 0x8289AED2, 0x8289BA66, 0x82884F12, 0x8289BA68, 0x8289AED4, 0x82885144, 0x82884F14, 0x8289BA6A, 0x8289AED6, 0x82885146, 0x82884F16};
  for (uint32_t address : kClearedWords) REX_STORE_U16(address, 0);
  for (uint32_t address : {0x82885498u, 0x8288549Cu, 0x828854A0u, 0x828854A4u}) REX_STORE_U32(address, 1);
  // The wipe would also clear collected fruit, upgrades and unlocks, so they are set back before the level loads and hides what was already picked up.
  const std::vector<uint32_t> kept = engine::DoneProgressEvents(ctx, base);
  CallGuest(ctx, base, sub_822CEA38, uint32_t(task), 1);
  engine::RestoreProgressEvents(ctx, base, kept);
  CallGuest(ctx, base, sub_822CCD98, uint32_t(task));
  REX_STORE_U32(0x8289B548, 0);
  REX_STORE_U32(0x8289B54C, 0);
  REXLOG_INFO("[kameo] level select: started task {}", task);
}

void ChangeToLevel(PPCContext& ctx, uint8_t* base, const std::string& level, uint32_t setup_point) {
  // In guest memory of its own: LevelChange copies the name, but only once its fade has started.
  static uint32_t name = 0;
  if (!name) name = rex::system::kernel_state()->memory()->SystemHeapAlloc(64);
  if (!name || level.empty()) return;
  WriteGuestStringAt(base, name, level.substr(0, 42).c_str());
  // As a level exit does (doLevelChangeAction): the setup point by number, and Kai as the player.
  REX_STORE_U32(engine::kNextLevelPlayerSetup, setup_point);
  WriteGuestStringAt(base, engine::kNextLevelPlayerObject, "Kai");
  // LevelChange, through its hook (current_level.cpp) so the level is tracked: fades out and loads the level whose file
  // is the ELF hash of its name (databaseMakeLevelFilename).
  CallGuest(ctx, base, sub_820B6B18, name);
  REXLOG_INFO("[kameo] level change to {} at setup point {}", level, setup_point);
}

}  // namespace kameo::ui

REXCVAR_DEFINE_INT32(kameo_dev_start_task, -1, "Kameo", "For testing: jump to this story task a few seconds after a level starts, as the pause menu's level select does. -1 is off. Keep it off with a save you care about.");

REXCVAR_DEFINE_STRING(kameo_dev_skin, "", "Kameo", "For testing: put Kameo in this skin, by file suffix (00 is Classic), some seconds after a level starts.");

REXCVAR_DEFINE_STRING(kameo_dev_start_level, "", "Kameo", "For testing: change to this level a few seconds after a level starts, by name (its file is the ELF hash of the name in hex, .lvl), with :N after it for its player setup point (0 when left out). Empty is off.");

void KameoDevStartTaskTick(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kTaskTick = 300;
  constexpr uint32_t kSkinTick = 900;
  static uint32_t ticks = 0;
  const int32_t task = REXCVAR_GET(kameo_dev_start_task);
  const std::string skin = REXCVAR_GET(kameo_dev_skin);
  const std::string target = REXCVAR_GET(kameo_dev_start_level);
  if (task < 0 && skin.empty() && target.empty()) return;
  const std::string level = KameoCurrentLevelName();
  if (level.empty() || level == "FrontEndLevel") return;
  const bool counting = ticks <= kSkinTick;
  if (counting) ++ticks;
  if (counting && ticks == kTaskTick + 1 && task >= 0) kameo::ui::JumpToTask(ctx, base, task);
  // A level change only once the level has been running a while: one made while another is still loading never finishes.
  static std::string target_from;
  static int64_t level_since = 0;
  if (level != target_from) {
    target_from = level;
    level_since = KameoSteadyMilliseconds();
  }
  static bool target_done = false;
  if (kameo::gpu::detail::LoadingScreenShowing()) level_since = KameoSteadyMilliseconds();
  if (!target_done && !target.empty() && KameoSteadyMilliseconds() - level_since > 10000) {
    target_done = true;
    const size_t colon = target.find(':');
    const uint32_t setup = colon == std::string::npos ? 0 : uint32_t(std::strtoul(target.c_str() + colon + 1, nullptr, 10));
    kameo::ui::ChangeToLevel(ctx, base, target.substr(0, colon), setup);
  }
  if (counting && ticks == kSkinTick + 1 && !skin.empty()) QueueKameoDlcSuffix(skin);
}
