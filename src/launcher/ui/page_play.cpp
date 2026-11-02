// Play page: keyboard or gamepad, the display basics and a shortcut to the DLC page.

#include <string>
#include <vector>

#include <shobjidl.h>

#include "core/localization.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

using l10n::Tr;
using l10n::TrFormat;

namespace {

const wchar_t* Wide(const std::u16string& text) { return reinterpret_cast<const wchar_t*>(text.c_str()); }

std::filesystem::path PickGameFile() {
  std::filesystem::path picked;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return picked;
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(Wide(Tr(u"Choose your copy of Kameo: Elements of Power")));
  // A Games on Demand package has no extension, so it needs the second filter.
  const COMDLG_FILTERSPEC filters[] = {{Wide(Tr(u"Xbox 360 disc image (*.iso)")), L"*.iso"}, {Wide(Tr(u"All files, for a Games on Demand package")), L"*.*"}};
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
  ImGui::TextColored(kGold, "%s", Tr("Set Up Your Game"));
  ImGui::Separator();
  if (app.iso.running()) {
    ImGui::TextWrapped("%s", Tr("Unpacking your game. This can take a few minutes, and Play will be ready as soon as it finishes."));
    ImGui::ProgressBar(app.iso.progress(), ImVec2(-FLT_MIN, 0), app.iso.current().c_str());
    if (ImGui::Button(Tr("Cancel"))) app.iso.Cancel();
  } else {
    ImGui::TextWrapped("%s", Tr("Kameo: RePowered runs from your own copy of the game. Choose your Xbox 360 disc image (.iso) or Games on Demand package, and the launcher will unpack it for you."));
    if (ImGui::Button(Tr("Choose Your Game..."))) {
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
  PageTitle(app, Tr("Welcome back"), Tr("Choose how you want to play, then press Play."));
  // Newer points into the list, so the copy has to outlive it.
  const std::vector<BuildRelease> releases = app.builds.releases();
  if (const BuildRelease* newer = app.builds.Newer(releases, app.include_prereleases)) {
    ImGui::BeginChild("play_update", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    const std::string available = newer->prerelease ? TrFormat("Pre-release {0} is available.", {newer->tag}) : TrFormat("Kameo RePowered {0} is available.", {newer->tag});
    ImGui::TextColored(kGold, "%s", available.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.builds.busy());
    if (ImGui::Button(Tr("Download and Switch"))) app.builds.Activate(newer->tag);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr("See Builds"))) app.page = Page::Builds;
    ImGui::EndChild();
    ImGui::Spacing();
  }
  if (!app.has_game_files || app.iso.running()) GameFilesCard(app);
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float tile_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
  const ImVec2 tile(tile_width, ImGui::GetFontSize() * 5.2f);
  const bool keyboard = app.settings.GetInt("kameo_input_mode") != 1;
  if (Tile("keyboard", Tr("Keyboard & Mouse"), Tr("WASD to move, the mouse turns the camera and points in menus. Controllers still work."), keyboard, tile)) app.settings.SetInt("kameo_input_mode", 0);
  ImGui::SameLine();
  if (Tile("gamepad", Tr("Gamepad"), Tr("Play with a controller; the keyboard and mouse leave the game alone."), !keyboard, tile)) app.settings.SetInt("kameo_input_mode", 1);
  ImGui::Spacing();
  if (BeginSection("play_display", Tr("Display"))) {
    DisplayModeRow(app);
    ResolutionRow(app);
    BoolRow(app, Tr("Widescreen"), "kameo_gfx_widescreen", Tr("A wider view that fills the screen instead of the original 16:9 picture."));
    // Each language is named in itself, so a player can find their own whatever the launcher is showing.
    ChoiceRow(app, Tr("Language"), "kameo_language", {{Tr("Game Default"), 0}, {"English", 1}, {"日本語", 2}, {"Deutsch", 3}, {"Français", 4}, {"Español", 5}, {"Italiano", 6}, {"한국어", 7}, {"繁體中文", 8}});
  }
  EndSection();
  ImGui::Spacing();
  // Not a settings row: the explanation has to be on show, not behind a (?).
  ImGui::BeginChild("play_crash_reports", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("Crash Reports"));
  ImGui::Separator();
  ImGui::TextWrapped("%s", Tr("Completely optional. If the game crashes, it asks whether to send a report so the problem can be fixed, and nothing is sent unless you choose Send Report. Turn this off and it will never ask."));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(Tr("Offer to Send Crash Reports"));
  ImGui::SameLine();
  if (Toggle("crash_reports", app.settings.GetBool("crash_reports"))) app.settings.SetBool("crash_reports", !app.settings.GetBool("crash_reports"));
  ImGui::EndChild();
  ImGui::Spacing();
  ImGui::BeginChild("play_dlc", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("Downloadable Content"));
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  const std::string installed = app.installed.empty() ? std::string(Tr("No DLC installed yet.")) : TrFormat("{0} package(s) installed.", {std::to_string(app.installed.size())});
  ImGui::TextUnformatted(installed.c_str());
  ImGui::SameLine();
  if (ImGui::Button(Tr("Manage DLC"))) app.page = Page::Dlc;
  ImGui::EndChild();
}

}  // namespace kameo::launcher
