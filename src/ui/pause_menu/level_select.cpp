// Level Select: the story's tasks grouped by region, and starting one the way the storybook's task jump does.

#include <string>
#include <vector>

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
    {u"Thorn's Castle", 1, 13},       {u"Enchanted Kingdom", 14, 31}, {u"The Badlands", 32, 36},     {u"Forgotten Forest", 37, 64},
    {u"Forest Temple", 65, 68},       {u"Mountain Falls", 69, 84},    {u"Mountain Rise", 85, 107},   {u"Water Temple", 108, 125},
    {u"Snow Top Village", 126, 152},  {u"Snow Temple", 153, 168},     {u"Ogre's Swamp", 169, 184},   {u"Thorn's Pass", 185, 202},
    {u"Thorn's Airship", 203, 225},
};

uint32_t g_levelChapter = 0;
uint32_t g_levelPickedTask = 0;
// A task to start once the pause menu has closed, or -1.
int32_t g_levelJumpTask = -1;

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

uint32_t PickedTaskIndex() { return g_levelPickedTask - kLevelChapters[g_levelChapter].first_task; }

}  // namespace

bool AddLevelSelectRows(PPCContext& ctx, uint8_t* base, uint32_t window, Page page) {
  if (page == Page::LevelSelect) {
    AddPageTitle(ctx, base, window, u"Level Select");
    for (uint32_t i = 0; i < std::size(kLevelChapters); ++i) AddCategoryLine(ctx, base, window, kItemLevelChapter + i, kLevelChapters[i].title);
    return true;
  }
  if (page == Page::LevelChapter) {
    const LevelChapter& chapter = kLevelChapters[g_levelChapter];
    AddPageTitle(ctx, base, window, chapter.title);
    for (uint32_t task = chapter.first_task; task <= chapter.last_task; ++task) AddCategoryLine(ctx, base, window, kItemLevelTask + task, Number(int32_t(task - chapter.first_task + 1), u". ") + TaskName(ctx, base, task));
    return true;
  }
  if (page == Page::ConfirmJump) {
    AddPageTitle(ctx, base, window, u"Start from \"" + TaskName(ctx, base, g_levelPickedTask) + u"\"? The story is set to this point; collectibles are kept.");
    AddCategoryLine(ctx, base, window, kItemJumpStart, u"Start");
    AddCategoryLine(ctx, base, window, kItemJumpCancel, u"Cancel");
    return true;
  }
  return false;
}

bool PressLevelSelectItem(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t id) {
  if (id == kItemLevelSelectPage) {
    ActivatePage(ctx, base, pause, window, Page::LevelSelect);
    return true;
  }
  if (id >= kItemLevelChapter && id < kItemLevelChapter + std::size(kLevelChapters)) {
    g_levelChapter = id - kItemLevelChapter;
    ActivatePage(ctx, base, pause, window, Page::LevelChapter);
    return true;
  }
  if (id > kItemLevelTask && id <= kItemLevelTask + kLevelChapters[std::size(kLevelChapters) - 1].last_task) {
    g_levelPickedTask = id - kItemLevelTask;
    ActivatePage(ctx, base, pause, window, Page::ConfirmJump);
    // The cursor starts on Cancel, so a stray second press does not replace anything.
    KeepCursor(ctx, base, window, 1);
    return true;
  }
  if (id == kItemJumpStart) {
    g_levelJumpTask = int32_t(g_levelPickedTask);
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

void StartPendingLevelJump(PPCContext& ctx, uint8_t* base) {
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

}  // namespace kameo::ui
