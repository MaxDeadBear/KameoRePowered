// The launcher frame: sidebar navigation, the Play button, the current page and the status footer.

#include <algorithm>
#include <cmath>
#include <string>

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {
namespace {

void NavButton(App& app, Page page, const char* label) {
  const bool selected = app.page == page;
  const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.35f);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton(label, size)) {
    app.page = page;
    app.capture_target.clear();
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (selected || ImGui::IsItemHovered()) draw->AddRectFilled(at, at + size, selected ? IM_COL32(90, 64, 170, 170) : IM_COL32(70, 50, 140, 110), 10.0f);
  if (selected) draw->AddRectFilled(at + ImVec2(0, size.y * 0.2f), at + ImVec2(4, size.y * 0.8f), ImGui::GetColorU32(kGold), 2.0f);
  draw->AddText(at + ImVec2(ImGui::GetStyle().FramePadding.x * 1.6f, (size.y - ImGui::GetFontSize()) * 0.5f), selected ? ImGui::GetColorU32(kGold) : ImGui::GetColorU32(kText), label);
}

}  // namespace

void DrawFrame(App& app) {
  const ImGuiIO& io = ImGui::GetIO();
  DrawBackground(ImGui::GetBackgroundDrawList(), io.DisplaySize, float(ImGui::GetTime()));
  for (std::string& line : app.installer.TakeResults()) app.dlc_log.push_back(std::move(line));
  static bool was_installing = false;
  if (was_installing && !app.installer.running()) RefreshDlc(app);
  was_installing = app.installer.running();
  PollCapture(app);
  if (app.builds.TakeActiveChanged()) {
    app.active_build = app.builds.active();
    SavePrefs(app);
  }
  if (std::string message = app.builds.TakeMessage(); !message.empty()) app.status = std::move(message);

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  const float sidebar = ImGui::GetFontSize() * 13.5f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.04f, 0.13f, 0.72f));
  ImGui::BeginChild("sidebar", ImVec2(sidebar, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleColor();
  ImGui::PushFont(app.heading, 44.0f);
  ImGui::TextColored(kGold, "KAMEO");
  ImGui::PopFont();
  ImGui::TextColored(kTextDim, "Elements of Power");
  ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "RePowered");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  NavButton(app, Page::Play, "Play");
  NavButton(app, Page::Video, "Video");
  NavButton(app, Page::Audio, "Audio");
  NavButton(app, Page::Controls, "Controls");
  NavButton(app, Page::Bindings, "Key Bindings");
  NavButton(app, Page::Mods, "Mods");
  NavButton(app, Page::Dlc, "DLC");
  NavButton(app, Page::Builds, "Builds");

  const float play_height = ImGui::GetFontSize() * 3.0f;
  const float footer = play_height + ImGui::GetFrameHeightWithSpacing() * 1.2f + ImGui::GetStyle().WindowPadding.y;
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - footer));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImVec2 size(ImGui::GetContentRegionAvail().x, play_height);
  // A build switch replaces the game's files, so the game cannot start while one runs.
  const bool busy = app.installer.running() || app.builds.busy();
  ImGui::BeginDisabled(busy);
  const bool play = ImGui::InvisibleButton("play", size);
  ImGui::EndDisabled();
  const bool hovered = ImGui::IsItemHovered() && !busy;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.2f);
  for (int ring = 4; ring > 0; --ring) draw->AddRect(at - ImVec2(float(ring), float(ring)), at + size + ImVec2(float(ring), float(ring)), IM_COL32(255, 200, 90, int((hovered ? 50 : 22) * pulse / ring)), 14.0f, 0, 2.0f);
  draw->AddRectFilledMultiColor(at, at + size, busy ? IM_COL32(90, 80, 90, 255) : IM_COL32(255, 214, 120, 255), busy ? IM_COL32(90, 80, 90, 255) : IM_COL32(255, 214, 120, 255), busy ? IM_COL32(60, 50, 60, 255) : IM_COL32(196, 128, 40, 255), busy ? IM_COL32(60, 50, 60, 255) : IM_COL32(196, 128, 40, 255));
  draw->AddRect(at, at + size, IM_COL32(255, 240, 200, hovered ? 255 : 150), 0.0f, 0, 1.5f);
  ImGui::PushFont(app.heading, 34.0f);
  const ImVec2 label_size = ImGui::CalcTextSize("PLAY");
  draw->AddText(at + (size - label_size) * 0.5f + ImVec2(0, 1), IM_COL32(40, 20, 10, 255), "PLAY");
  ImGui::PopFont();
  if (play && LaunchGame(app) && app.close_on_launch) app.quit = true;
  if (ImGui::Checkbox("Close launcher on Play", &app.close_on_launch)) SavePrefs(app);
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None);
  ImGui::BeginChild("page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_AlwaysUseWindowPadding);
  switch (app.page) {
    case Page::Play: PagePlay(app); break;
    case Page::Video: PageVideo(app); break;
    case Page::Audio: PageAudio(app); break;
    case Page::Controls: PageControls(app); break;
    case Page::Bindings: PageBindings(app); break;
    case Page::Mods: PageMods(app); break;
    case Page::Dlc: PageDlc(app); break;
    case Page::Builds: PageBuilds(app); break;
  }
  ImGui::EndChild();
  const std::string footer_text = !app.settings.error().empty() ? app.settings.error() : !app.status.empty() ? app.status : "Settings are saved as you change them to " + app.settings.path().filename().string() + ".";
  ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
  ImGui::TextColored(app.settings.error().empty() ? kTextDim : ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", footer_text.c_str());
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace kameo::launcher
