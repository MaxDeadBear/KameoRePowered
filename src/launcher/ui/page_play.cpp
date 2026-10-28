// Play page: keyboard or gamepad, the display basics and a shortcut to the DLC page.

#include <vector>

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

void PagePlay(App& app) {
  PageTitle(app, "Welcome back", "Choose how you want to play, then press Play.");
  // Newer points into the list, so the copy has to outlive it.
  const std::vector<BuildRelease> releases = app.builds.releases();
  if (const BuildRelease* newer = app.builds.Newer(releases, app.include_prereleases)) {
    ImGui::BeginChild("play_update", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGold, "%s %s is available.", newer->prerelease ? "Pre-release" : "Kameo RePowered", newer->tag.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.builds.busy());
    if (ImGui::Button("Download and Switch")) app.builds.Activate(newer->tag);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("See Builds")) app.page = Page::Builds;
    ImGui::EndChild();
    ImGui::Spacing();
  }
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float tile_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
  const ImVec2 tile(tile_width, ImGui::GetFontSize() * 5.2f);
  const bool keyboard = app.settings.GetInt("kameo_input_mode") != 1;
  if (Tile("keyboard", "Keyboard & Mouse", "WASD to move, the mouse turns the camera and points in menus. Controllers still work.", keyboard, tile)) app.settings.SetInt("kameo_input_mode", 0);
  ImGui::SameLine();
  if (Tile("gamepad", "Gamepad", "Play with a controller; the keyboard and mouse leave the game alone.", !keyboard, tile)) app.settings.SetInt("kameo_input_mode", 1);
  ImGui::Spacing();
  if (BeginSection("play_display", "Display")) {
    DisplayModeRow(app);
    ResolutionRow(app);
    BoolRow(app, "Widescreen", "kameo_gfx_widescreen", "A wider view that fills the screen instead of the original 16:9 picture.");
    ChoiceRow(app, "Language", "kameo_language", {{"Game Default", 0}, {"English", 1}, {"Japanese", 2}, {"German", 3}, {"French", 4}, {"Spanish", 5}, {"Italian", 6}, {"Korean", 7}, {"Chinese", 8}});
  }
  EndSection();
  ImGui::BeginChild("play_dlc", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Downloadable Content");
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  ImGui::Text(app.installed.empty() ? "No DLC installed yet." : "%d package(s) installed.", int(app.installed.size()));
  ImGui::SameLine();
  if (ImGui::Button("Manage DLC")) app.page = Page::Dlc;
  ImGui::EndChild();
}

}  // namespace kameo::launcher
