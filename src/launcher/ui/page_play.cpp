// Play page: keyboard or gamepad, the display basics and a shortcut to the DLC page.

#include <vector>

#include <shobjidl.h>

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {
namespace {

std::filesystem::path PickGameFile() {
  std::filesystem::path picked;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return picked;
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Choose your copy of Kameo: Elements of Power");
  // A Games on Demand package has no extension, so it needs the second filter.
  const COMDLG_FILTERSPEC filters[] = {{L"Xbox 360 disc image (*.iso)", L"*.iso"}, {L"All files, for a Games on Demand package", L"*.*"}};
  dialog->SetFileTypes(2, filters);
  if (SUCCEEDED(dialog->Show(::GetActiveWindow()))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        picked = path;
        ::CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return picked;
}

// Shown until the game has files to run: picks the player's copy and unpacks it.
void GameFilesCard(App& app) {
  ImGui::BeginChild("play_game_files", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Set Up Your Game");
  ImGui::Separator();
  if (app.iso.running()) {
    ImGui::TextWrapped("Unpacking your game. This can take a few minutes, and Play will be ready as soon as it finishes.");
    ImGui::ProgressBar(app.iso.progress(), ImVec2(-FLT_MIN, 0), app.iso.current().c_str());
    if (ImGui::Button("Cancel")) app.iso.Cancel();
  } else {
    ImGui::TextWrapped("Kameo: RePowered runs from your own copy of the game. Choose your Xbox 360 disc image (.iso) or Games on Demand package, and the launcher will unpack it for you.");
    if (ImGui::Button("Choose Your Game...")) {
      if (const std::filesystem::path picked = PickGameFile(); !picked.empty()) {
        app.game_files_message.clear();
        app.iso.Start(picked, app.folder / "assets");
      }
    }
    if (!app.game_files_message.empty()) ImGui::TextColored(kGold, "%s", app.game_files_message.c_str());
  }
  ImGui::EndChild();
  ImGui::Spacing();
}

}  // namespace

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
  if (!app.has_game_files || app.iso.running()) GameFilesCard(app);
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
  ImGui::Spacing();
  // Not a settings row: the explanation has to be on show, not behind a (?).
  ImGui::BeginChild("play_crash_reports", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Crash Reports");
  ImGui::Separator();
  ImGui::TextWrapped("Completely optional. If the game crashes, it asks whether to send a report so the problem can be fixed, and nothing is sent unless you choose Send Report. Turn this off and it will never ask.");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Offer to Send Crash Reports");
  ImGui::SameLine();
  if (Toggle("crash_reports", app.settings.GetBool("crash_reports"))) app.settings.SetBool("crash_reports", !app.settings.GetBool("crash_reports"));
  ImGui::EndChild();
  ImGui::Spacing();
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
