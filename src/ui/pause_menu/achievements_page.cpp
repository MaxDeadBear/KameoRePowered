// Achievements: the title's achievements as the game's own menu rows, in place of the SDK's ImGui list.

#include <mutex>
#include <string>
#include <vector>

#include <rex/system/kernel_state.h>

#include "ui/pause_menu/pause_menu.h"
#include "ui/pause_menu/pause_menu_internal.h"

namespace kameo::ui {
namespace {

// What the render thread's icon layer reads: the page's window and its rows, published when the page is built and withdrawn when another page is.
std::mutex g_iconRowsMutex;
uint32_t g_iconTextObject = 0;
std::vector<KameoAchievementIconRow> g_iconRows;

constexpr const char16_t* kDescriptionStyle = u"{size='16'}";
constexpr const char16_t* kSummaryStyle = u"{alignment='centre' size='21'}";
// Room at the left of each row for the icon the render thread draws inside the box, plus a gap; the description's smaller glyphs need more spaces for the same width.
constexpr const char16_t* kLabelIndent = u"           ";
constexpr const char16_t* kDescriptionIndent = u"              ";
// A description longer than this wraps, and the wrapped tail starts under the icon column; the catalog's longest is 47 characters.
constexpr size_t kDescriptionMaxChars = 58;

std::u16string FitOneLine(std::u16string text) {
  if (text.size() <= kDescriptionMaxChars) return text;
  text.resize(kDescriptionMaxChars - 3);
  return text + u"...";
}

std::u16string Widen(const std::string& text) {
  // The XDBF strings are UTF-8; everything the catalog holds is Latin script, so the code points carry over one by one.
  std::u16string wide;
  wide.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    uint32_t point = c;
    size_t length = 1;
    if (c >= 0xF0) { point = c & 0x07; length = 4; }
    else if (c >= 0xE0) { point = c & 0x0F; length = 3; }
    else if (c >= 0xC0) { point = c & 0x1F; length = 2; }
    for (size_t k = 1; k < length && i + k < text.size(); ++k) point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    i += length;
    if (point >= 0x10000) {
      point -= 0x10000;
      wide.push_back(char16_t(0xD800 + (point >> 10)));
      wide.push_back(char16_t(0xDC00 + (point & 0x3FF)));
    } else {
      wide.push_back(char16_t(point));
    }
  }
  return wide;
}

}  // namespace

bool AddAchievementRows(PPCContext& ctx, uint8_t* base, uint32_t window, Page page) {
  if (page != Page::Achievements) return false;
  AddPageTitle(ctx, base, window, u"Achievements");
  rex::system::KernelState* kernel = rex::system::kernel_state();
  const std::vector<rex::system::AchievementInfo> achievements = kernel ? kernel->achievements().ListAchievements() : std::vector<rex::system::AchievementInfo>{};
  if (achievements.empty()) {
    if (const uint32_t note = GuestWideString(u"No achievement data was found in the game's files.")) AddTextItem(ctx, base, window, 0, GuestWideString(kSummaryStyle), note, 0, false);
    return true;
  }
  uint32_t unlocked = 0;
  uint32_t score = 0;
  uint32_t total_score = 0;
  for (const auto& a : achievements) {
    total_score += a.gamerscore;
    if (kernel->achievements().IsUnlocked(a.id)) {
      ++unlocked;
      score += a.gamerscore;
    }
  }
  const std::u16string summary = Number(int32_t(unlocked), u" of ") + Number(int32_t(achievements.size()), u" unlocked   ") + Number(int32_t(score), u"G of ") + Number(int32_t(total_score), u"G");
  if (const uint32_t note = GuestWideString(summary)) AddTextItem(ctx, base, window, 0, GuestWideString(kSummaryStyle), note, 0, false);
  // Rows are selectable so the list scrolls like any page; the selected index is a byte, so the list is capped where it would wrap.
  std::vector<KameoAchievementIconRow> icon_rows;
  uint32_t row = 0;
  for (const auto& a : achievements) {
    if (row >= 200) break;
    const bool is_unlocked = kernel->achievements().IsUnlocked(a.id);
    icon_rows.push_back({a.id, is_unlocked});
    // The gamerscore is the row's value; locked or unlocked shows in the icon drawn beside the row, dimmed or not.
    AddToggleRow(ctx, base, window, kItemAchievementRow + row++, std::u16string(kLabelIndent) + Widen(a.label), GuestWideString(Number(int32_t(a.gamerscore), u"G")));
    const std::string& description = is_unlocked || a.unachieved_description.empty() ? a.description : a.unachieved_description;
    if (!description.empty()) {
      if (const uint32_t text = GuestWideString(std::u16string(kDescriptionStyle) + kDescriptionIndent + FitOneLine(Widen(description)))) AddTextItem(ctx, base, window, 0, 0, text, 0, false);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_iconRowsMutex);
    g_iconRows = std::move(icon_rows);
  }
  return true;
}

bool PressAchievementItem(uint32_t id) { return id >= kItemAchievementRow && id < kItemAchievementRow + 256; }

void NoteActivatedPage(Page page, uint32_t text_object) {
  std::lock_guard<std::mutex> lock(g_iconRowsMutex);
  if (page != Page::Achievements) {
    g_iconTextObject = 0;
    g_iconRows.clear();
    return;
  }
  g_iconTextObject = text_object;
}

}  // namespace kameo::ui

bool KameoAchievementsPageShowing(uint32_t* text_object, std::vector<KameoAchievementIconRow>* rows) {
  std::lock_guard<std::mutex> lock(kameo::ui::g_iconRowsMutex);
  if (!kameo::ui::g_iconTextObject || kameo::ui::g_iconRows.empty()) return false;
  *text_object = kameo::ui::g_iconTextObject;
  *rows = kameo::ui::g_iconRows;
  return true;
}
