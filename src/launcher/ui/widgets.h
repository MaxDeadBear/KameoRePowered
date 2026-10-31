// The launcher's look and its building blocks: theme, fonts, the night-sky background and the settings rows every page is made of.
#pragma once

#include <initializer_list>
#include <utility>

#include "launcher/app.h"

namespace kameo::launcher {

struct Choice {
  const char* label;
  int value;
};

void ApplyTheme(float scale);
void LoadFonts(WindowState& window);
void DrawBackground(ImDrawList* draw, ImVec2 size, float time);

bool Toggle(const char* id, bool value);
void Help(const char* text);
void PageTitle(App& app, const char* title, const char* subtitle);
// A bordered card with a gold title over a two-column table of rows; EndSection must follow whatever BeginSection returns.
bool BeginSection(const char* id, const char* title);
void EndSection();
void Row(const char* label, const char* help = nullptr);
void BoolRow(App& app, const char* label, const char* name, const char* help = nullptr);
void ChoiceRow(App& app, const char* label, const char* name, std::initializer_list<Choice> choices, const char* help = nullptr);
void BoolChoiceRow(App& app, const char* label, const char* name, const char* off, const char* on, const char* help = nullptr);
void StringChoiceRow(App& app, const char* label, const char* name, std::initializer_list<std::pair<const char*, const char*>> choices, const char* help = nullptr);
void IntSliderRow(App& app, const char* label, const char* name, int minimum, int maximum, const char* format, const char* help = nullptr);
void RestoreButton(App& app, std::initializer_list<const char*> names);
void DisplayModeRow(App& app);
void ResolutionRow(App& app);
// A big selectable tile, used for the input mode on the Play page.
bool Tile(const char* id, const char* title, const char* detail, bool selected, ImVec2 size);

}  // namespace kameo::launcher
