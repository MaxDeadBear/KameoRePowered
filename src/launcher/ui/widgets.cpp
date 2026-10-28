// The settings rows, toggles, tiles and section cards the launcher pages are built from.

#include <cmath>
#include <cstdlib>
#include <string>

#include "launcher/ui/widgets.h"

namespace kameo::launcher {
namespace {

struct Resolution {
  int width;
  int height;
};

constexpr Resolution kResolutions[] = {{0, 0}, {640, 360}, {854, 480}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {2560, 1080}, {3440, 1440}, {5120, 2160}};

// A table that is scrolled out of view is never begun, so EndSection only ends one that was.
bool g_sectionTable = false;

int WindowMode(App& app) {
  const int mode = app.settings.GetInt("kameo_window_mode");
  if (mode >= 0 && mode <= 2) return mode;
  return ReadMainConfigFullscreen(app.folder / "kameorepowered.toml") ? 1 : 0;
}

}  // namespace

bool Toggle(const char* id, bool value) {
  const float height = ImGui::GetFrameHeight() * 0.8f;
  const float width = height * 1.9f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, ImGui::GetFrameHeight()));
  const float top = at.y + (ImGui::GetFrameHeight() - height) * 0.5f;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 track = value ? ImGui::GetColorU32(ImVec4(kGold.x * 0.85f, kGold.y * 0.85f, kGold.z * 0.85f, 1.0f)) : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
  draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + width, top + height), track, height * 0.5f);
  const float knob = value ? at.x + width - height * 0.5f : at.x + height * 0.5f;
  draw->AddCircleFilled(ImVec2(knob, top + height * 0.5f), height * 0.5f - 3.0f, value ? IM_COL32(40, 24, 70, 255) : IM_COL32(220, 215, 240, 255));
  return clicked;
}

void Help(const char* text) {
  if (!text || !*text) return;
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void PageTitle(App& app, const char* title, const char* subtitle) {
  ImGui::PushFont(app.heading, 30.0f);
  ImGui::TextColored(kGold, "%s", title);
  ImGui::PopFont();
  if (subtitle) ImGui::TextColored(kTextDim, "%s", subtitle);
  ImGui::Spacing();
}

bool BeginSection(const char* id, const char* title) {
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", title);
  ImGui::Separator();
  g_sectionTable = ImGui::BeginTable("rows", 2, ImGuiTableFlags_SizingStretchProp);
  return g_sectionTable;
}

void EndSection() {
  if (g_sectionTable) ImGui::EndTable();
  g_sectionTable = false;
  ImGui::EndChild();
  ImGui::Spacing();
}

void Row(const char* label, const char* help) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  Help(help);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

void BoolRow(App& app, const char* label, const char* name, const char* help) {
  Row(label, help);
  if (Toggle(name, app.settings.GetBool(name))) app.settings.SetBool(name, !app.settings.GetBool(name));
}

void ChoiceRow(App& app, const char* label, const char* name, std::initializer_list<Choice> choices, const char* help) {
  Row(label, help);
  const int current = app.settings.GetInt(name);
  const Choice* selected = choices.begin();
  for (const Choice& choice : choices) {
    if (std::abs(choice.value - current) < std::abs(selected->value - current)) selected = &choice;
  }
  if (ImGui::BeginCombo((std::string("##") + name).c_str(), selected->label)) {
    for (const Choice& choice : choices) {
      if (ImGui::Selectable(choice.label, &choice == selected)) app.settings.SetInt(name, choice.value);
    }
    ImGui::EndCombo();
  }
}

void BoolChoiceRow(App& app, const char* label, const char* name, const char* off, const char* on, const char* help) {
  Row(label, help);
  const bool value = app.settings.GetBool(name);
  if (ImGui::BeginCombo((std::string("##") + name).c_str(), value ? on : off)) {
    if (ImGui::Selectable(off, !value)) app.settings.SetBool(name, false);
    if (ImGui::Selectable(on, value)) app.settings.SetBool(name, true);
    ImGui::EndCombo();
  }
}

void StringChoiceRow(App& app, const char* label, const char* name, std::initializer_list<std::pair<const char*, const char*>> choices, const char* help) {
  Row(label, help);
  const std::string current = app.settings.Get(name);
  const char* shown = choices.begin()->first;
  for (const auto& [text, value] : choices) {
    if (current == value) shown = text;
  }
  if (ImGui::BeginCombo((std::string("##") + name).c_str(), shown)) {
    for (const auto& [text, value] : choices) {
      if (ImGui::Selectable(text, current == value)) app.settings.Set(name, value);
    }
    ImGui::EndCombo();
  }
}

void IntSliderRow(App& app, const char* label, const char* name, int minimum, int maximum, const char* format, const char* help) {
  Row(label, help);
  int value = app.settings.GetInt(name);
  ImGui::SliderInt((std::string("##") + name).c_str(), &value, minimum, maximum, format);
  if (value != app.settings.GetInt(name)) app.settings.SetInt(name, value);
}

void RestoreButton(App& app, std::initializer_list<const char*> names) {
  const float width = ImGui::CalcTextSize("Restore Defaults").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - width);
  if (ImGui::Button("Restore Defaults")) {
    for (const char* name : names) app.settings.Reset(name);
    app.status = "Restored the defaults on this page.";
  }
}

void DisplayModeRow(App& app) {
  Row("Display Mode");
  static const char* kModes[] = {"Windowed", "Borderless", "Fullscreen"};
  const int mode = WindowMode(app);
  if (ImGui::BeginCombo("##window_mode", kModes[mode])) {
    for (int i = 0; i < 3; ++i) {
      if (ImGui::Selectable(kModes[i], i == mode)) app.settings.SetInt("kameo_window_mode", i);
    }
    ImGui::EndCombo();
  }
}

void ResolutionRow(App& app) {
  Row("Resolution", "The size the game renders at. A size of another shape than your window is shown with black bars.");
  const int width = app.settings.GetInt("kameo_gfx_render_width");
  const int height = app.settings.GetInt("kameo_gfx_render_height");
  const auto label = [&](const Resolution& r) { return r.height <= 0 ? std::string("Match Window") : std::to_string(r.width) + " x " + std::to_string(r.height) + (r.width * 9 > r.height * 17 ? "  (Ultrawide)" : ""); };
  size_t current = 0;
  for (size_t i = 1; i < std::size(kResolutions); ++i) {
    if (height > 0 && kResolutions[i].height == height && (kResolutions[i].width == width || width <= 0)) current = i;
  }
  if (ImGui::BeginCombo("##resolution", label(kResolutions[current]).c_str())) {
    for (size_t i = 0; i < std::size(kResolutions); ++i) {
      if (ImGui::Selectable(label(kResolutions[i]).c_str(), i == current)) {
        app.settings.SetInt("kameo_gfx_render_width", kResolutions[i].width);
        app.settings.SetInt("kameo_gfx_render_height", kResolutions[i].height);
      }
    }
    ImGui::EndCombo();
  }
}

// A big selectable tile, used for the input mode on the Play page.
bool Tile(const char* id, const char* title, const char* detail, bool selected, ImVec2 size) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, size);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 fill = selected ? IM_COL32(92, 64, 170, 230) : hovered ? IM_COL32(60, 44, 120, 220) : IM_COL32(36, 28, 80, 200);
  draw->AddRectFilled(at, at + size, fill, 12.0f);
  draw->AddRect(at, at + size, selected ? ImGui::GetColorU32(kGold) : IM_COL32(140, 115, 220, 90), 12.0f, 0, selected ? 2.5f : 1.0f);
  const ImVec2 padding = ImGui::GetStyle().WindowPadding;
  draw->AddText(nullptr, ImGui::GetFontSize() * 1.25f, at + padding, selected ? ImGui::GetColorU32(kGold) : ImGui::GetColorU32(kText), title);
  draw->AddText(nullptr, ImGui::GetFontSize() * 0.9f, at + padding + ImVec2(0, ImGui::GetFontSize() * 1.7f), ImGui::GetColorU32(kTextDim), detail, nullptr, size.x - padding.x * 2);
  return clicked;
}

}  // namespace kameo::launcher
