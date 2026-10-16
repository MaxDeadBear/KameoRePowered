// What the mouse pointer knows about the game's menus: the UI tick, the open pause window, windows drawn onto the storybook's pages and where their lines were drawn.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kameo::ui {

struct DrawnLine {
  int32_t index;
  float top;
  float bottom;
};

// Counts game ticks; recorded windows and lines older than a few ticks are stale.
uint32_t UiTick();
// Once per game tick: ages the pause window, advances the UI tick and now and then forgets old windows.
void TickMenuUiState();

// The pause menu's window as its tick last saw it; a closed menu stops taking the pointer after two ticks.
void NotePauseMenuWindow(uint32_t window);
bool IsLivePauseMenuWindow(uint32_t window);

// Text objects xfix_textWindow_render drew into the storybook's page texture in the last few ticks.
void NoteBookPageWindow(uint32_t text_object);
bool RenderedOntoBookPage(uint32_t text_object);

// Where xfix_textWindow_render last drew each selectable line of a window, in layout units.
void StoreDrawnLines(uint32_t text_object, std::vector<DrawnLine> lines);
// The selectable line whose drawn centre is nearest y in the window's last render, within one line height of it; -2 when nothing recent was recorded.
int32_t DrawnLineAt(uint32_t text_object, float y);
// How much taller than measured a window's selectable lines are drawn.
float WindowTextScale(uint8_t* base, uint32_t text_object);

// Maps a layout point onto the page texture through the open book's page meshes.
bool BookPagePoint(uint8_t* base, float layout_x, float layout_y, float* page_x, float* page_y);

}  // namespace kameo::ui
