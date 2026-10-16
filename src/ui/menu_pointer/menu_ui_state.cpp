#include "ui/menu_pointer/menu_ui_state.h"

#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include <fmt/format.h>

namespace kameo::ui {

namespace {

struct DrawnLines {
  std::vector<DrawnLine> lines;
  uint32_t tick = 0;
};

std::atomic<uint32_t> g_uiTick{0};
uint32_t g_pauseWindow = 0;
uint32_t g_pauseWindowTicks = 1000;
std::mutex g_bookPageWindowsMutex;
std::unordered_map<uint32_t, uint32_t> g_bookPageWindows;
std::mutex g_drawnLinesMutex;
std::unordered_map<uint32_t, DrawnLines> g_drawnLines;

void AdvanceUiTick() {
  const uint32_t tick = g_uiTick.fetch_add(1) + 1;
  if (tick % 300 != 0) return;
  {
    std::lock_guard<std::mutex> lock(g_bookPageWindowsMutex);
    for (auto it = g_bookPageWindows.begin(); it != g_bookPageWindows.end();) it = tick - it->second > 60 ? g_bookPageWindows.erase(it) : std::next(it);
  }
  std::lock_guard<std::mutex> lock(g_drawnLinesMutex);
  for (auto it = g_drawnLines.begin(); it != g_drawnLines.end();) it = tick - it->second.tick > 60 ? g_drawnLines.erase(it) : std::next(it);
}

}  // namespace

uint32_t UiTick() { return g_uiTick.load(); }

void TickMenuUiState() {
  if (g_pauseWindowTicks < 1000) ++g_pauseWindowTicks;
  AdvanceUiTick();
}

void NotePauseMenuWindow(uint32_t window) {
  g_pauseWindow = window;
  g_pauseWindowTicks = 0;
}

bool IsLivePauseMenuWindow(uint32_t window) { return window == g_pauseWindow && g_pauseWindowTicks <= 2; }

void NoteBookPageWindow(uint32_t text_object) {
  std::lock_guard<std::mutex> lock(g_bookPageWindowsMutex);
  g_bookPageWindows[text_object] = g_uiTick.load();
}

bool RenderedOntoBookPage(uint32_t text_object) {
  std::lock_guard<std::mutex> lock(g_bookPageWindowsMutex);
  const auto found = g_bookPageWindows.find(text_object);
  return found != g_bookPageWindows.end() && g_uiTick.load() - found->second <= 5;
}

void StoreDrawnLines(uint32_t text_object, std::vector<DrawnLine> lines) {
  std::lock_guard<std::mutex> lock(g_drawnLinesMutex);
  DrawnLines& drawn = g_drawnLines[text_object];
  drawn.lines = std::move(lines);
  drawn.tick = g_uiTick.load();
}

// Nearest rather than strictly inside, because the book's pages flutter a few units away from the mapping.
int32_t DrawnLineAt(uint32_t text_object, float y) {
  std::lock_guard<std::mutex> lock(g_drawnLinesMutex);
  const auto found = g_drawnLines.find(text_object);
  if (found == g_drawnLines.end() || g_uiTick.load() - found->second.tick > 5) return -2;
  int32_t best = -1;
  float best_distance = 0.0f;
  for (const DrawnLine& line : found->second.lines) {
    const float height = line.bottom - line.top;
    const float distance = std::abs(y - (line.top + line.bottom) * 0.5f);
    if (distance > height || (best >= 0 && distance >= best_distance)) continue;
    best = line.index;
    best_distance = distance;
  }
  return best;
}

}  // namespace kameo::ui
