// Builds page: the game builds published on GitHub, which are downloaded, and switching the game to one of them.

#include <cstdio>
#include <string>

#include <windows.h>

#include <shellapi.h>

#include "core/localization.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

using l10n::Tr;
using l10n::TrFormat;

namespace {

constexpr ImVec4 kPrerelease = ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
constexpr ImVec4 kActive = ImVec4(0.55f, 0.95f, 0.6f, 1.0f);

std::string SizeText(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f MB", double(bytes) / (1024.0 * 1024.0));
  return text;
}

void OpenInBrowser(const std::string& url) {
  if (url.rfind("https://", 0) == 0) ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const InstalledBuild* FindInstalled(const std::vector<InstalledBuild>& installed, const std::string& tag) {
  for (const InstalledBuild& build : installed) {
    if (build.tag == tag) return &build;
  }
  return nullptr;
}

}  // namespace

void PageBuilds(App& app) {
  PageTitle(app, Tr("Builds"), Tr("Download new versions of the game and switch between them. Your settings, saves and game files stay as they are."));
  BuildManager& builds = app.builds;
  const bool busy = builds.busy();
  const std::vector<BuildRelease> releases = builds.releases();
  const std::vector<InstalledBuild> installed = builds.installed();
  const std::string active = builds.active();

  ImGui::BeginChild("builds_current", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("In Use"));
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  if (active.empty()) ImGui::TextWrapped("%s", Tr("The build in the game folder was installed by hand, so its version is not known. Switching to a build below keeps a copy of it first."));
  else ImGui::Text("%s", active.c_str());
  if (const BuildRelease* newer = builds.Newer(releases, app.include_prereleases)) {
    ImGui::TextColored(kGold, "%s", TrFormat("{0} is available.", {newer->tag}).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(Tr("Download and Switch"))) builds.Activate(newer->tag);
    ImGui::EndDisabled();
  } else if (builds.fetched()) {
    ImGui::TextColored(kTextDim, "%s", app.include_prereleases ? Tr("You have the newest build.") : Tr("You have the newest release."));
  }
  if (ImGui::Checkbox(Tr("Include pre-releases in update notices"), &app.include_prereleases)) SavePrefs(app);
  Help(Tr("Pre-releases are test builds with the newest fixes; releases are the ones considered stable. Both are always listed below."));
  if (ImGui::Checkbox(Tr("Check for new builds when the launcher opens"), &app.check_for_builds)) SavePrefs(app);
  ImGui::EndChild();

  if (busy) {
    ImGui::Spacing();
    const std::string task = builds.task();
    const float progress = builds.progress();
    ImGui::TextColored(kText, "%s", task.c_str());
    if (progress >= 0.0f) ImGui::ProgressBar(progress, ImVec2(-ImGui::GetFrameHeight() * 4.0f, 0));
    else ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(-ImGui::GetFrameHeight() * 4.0f, 0), "");
    ImGui::SameLine();
    if (ImGui::Button(Tr("Cancel"))) builds.Cancel();
  }

  ImGui::Spacing();
  ImGui::BeginChild("builds_list", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("Available Builds"));
  const char* refresh = Tr("Refresh");
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(refresh).x - ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetCursorPosX());
  ImGui::BeginDisabled(busy);
  if (ImGui::SmallButton(refresh)) builds.Refresh();
  ImGui::EndDisabled();
  ImGui::Separator();
  if (releases.empty()) {
    ImGui::TextColored(kTextDim, "%s", busy ? Tr("Looking for builds...") : builds.fetched() ? Tr("No builds have been published yet.") : Tr("Press Refresh to see the builds published on GitHub."));
  }
  if (!releases.empty() && ImGui::BeginTable("builds_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
    ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed);
    for (const BuildRelease& release : releases) {
      ImGui::PushID(release.tag.c_str());
      ImGui::TableNextRow();
      const InstalledBuild* here = FindInstalled(installed, release.tag);
      const bool in_use = release.tag == active;

      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(in_use ? kActive : kText, "%s", release.tag.c_str());
      if (release.prerelease) {
        ImGui::SameLine();
        ImGui::TextColored(kPrerelease, "%s", Tr("pre-release"));
      }
      if (!release.notes.empty() && ImGui::TreeNodeEx("notes", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", Tr("Notes"))) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kTextDim, "%s", release.notes.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton(Tr("Open on GitHub"))) OpenInBrowser(release.url);
        ImGui::TreePop();
      }

      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kTextDim, "%s", release.published.c_str());

      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (in_use) ImGui::TextColored(kActive, "%s", Tr("In use"));
      else if (here) ImGui::TextColored(kText, "%s", Tr("Downloaded"));
      else if (release.asset_url.empty()) ImGui::TextColored(kTextDim, "%s", Tr("No Windows build"));
      else ImGui::TextColored(kTextDim, "%s", SizeText(release.asset_size).c_str());

      ImGui::TableNextColumn();
      ImGui::BeginDisabled(busy);
      if (!in_use && !release.asset_url.empty()) {
        if (ImGui::Button(here ? Tr("Switch") : Tr("Download and Switch"))) builds.Activate(release.tag);
        if (!here) {
          ImGui::SameLine();
          if (ImGui::Button(Tr("Download"))) builds.Install(release.tag);
        }
      }
      if (here && !in_use) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("Delete"))) builds.Remove(release.tag);
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();

  // Builds kept from the game folder before a switch, so it can be undone.
  bool any_saved = false;
  for (const InstalledBuild& build : installed) any_saved = any_saved || build.saved_local;
  if (any_saved) {
    ImGui::Spacing();
    ImGui::BeginChild("builds_saved", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(kGold, "%s", Tr("Your Previous Builds"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", Tr("Copies of the build that was in the game folder before the launcher first switched it."));
    for (const InstalledBuild& build : installed) {
      if (!build.saved_local) continue;
      ImGui::PushID(build.tag.c_str());
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(build.tag == active ? kActive : kText, "%s", build.tag.c_str());
      ImGui::SameLine();
      ImGui::BeginDisabled(busy || build.tag == active);
      if (ImGui::Button(Tr("Switch"))) builds.Activate(build.tag);
      ImGui::SameLine();
      if (ImGui::Button(Tr("Delete"))) builds.Remove(build.tag);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndChild();
  }
}

}  // namespace kameo::launcher
