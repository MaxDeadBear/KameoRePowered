// DLC page: installing Xbox 360 DLC packages from a folder or files, and removing installed ones.

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h>
#include <shobjidl.h>

#include "core/localization.h"
#include "launcher/title_update.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

using l10n::Tr;
using l10n::TrFormat;

namespace {

const wchar_t* Wide(const std::u16string& text) { return reinterpret_cast<const wchar_t*>(text.c_str()); }

std::vector<std::filesystem::path> PickPaths(bool folders, const wchar_t* title) {
  std::vector<std::filesystem::path> picked;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return picked;
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT));
  dialog->SetTitle(title);
  if (SUCCEEDED(dialog->Show(::GetActiveWindow()))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dialog->GetResults(&items))) {
      DWORD count = 0;
      items->GetCount(&count);
      for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          picked.emplace_back(path);
          ::CoTaskMemFree(path);
        }
        item->Release();
      }
      items->Release();
    }
  }
  dialog->Release();
  return picked;
}

void StartInstall(App& app, const std::vector<std::filesystem::path>& picked) {
  std::vector<std::filesystem::path> packages;
  for (const auto& path : picked) {
    for (auto& package : FindPackages(path)) packages.push_back(std::move(package));
  }
  if (packages.empty()) {
    app.dlc_log.push_back(Tr("No Xbox 360 packages were found there. Pick the folder holding the DLC files (for example ...\\4D5307D2\\00000002)."));
    return;
  }
  app.installer.Start(std::move(packages), app.content_root, app.folder);
}

// Title updates are installer packages (content type 000B0000), kept apart from DLC on the console and in Xenia.
void StartTitleUpdateInstall(App& app, const std::vector<std::filesystem::path>& picked) {
  if (picked.empty()) return;
  std::vector<std::filesystem::path> packages;
  for (const auto& path : picked) {
    for (auto& package : FindPackages(path)) packages.push_back(std::move(package));
  }
  if (packages.empty()) {
    app.dlc_log.push_back(Tr("No Xbox 360 package was found there. The title update is a single file in ...\\4D5307D2\\000B0000."));
    return;
  }
  app.installer.Start(std::move(packages), app.content_root, app.folder);
}

// The title update can come in with the DLC from the buttons above or on its own from here; this also says whether it
// is in and lets it go again.
void TitleUpdateCard(App& app, bool busy) {
  ImGui::BeginChild("dlc_title_update", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("Title Update"));
  ImGui::Separator();
  if (TitleUpdateInstalled(app.folder)) {
    ImGui::TextWrapped("%s", Tr("Installed. The game starts with the title update: Expert mode, and Rune Battle and Time Attack with the DLC that carries their levels."));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(Tr("Remove Title Update"))) ImGui::OpenPopup("confirm_remove_update");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("confirm_remove_update")) {
      ImGui::TextUnformatted(Tr("Remove the title update? The game starts without it."));
      if (ImGui::Button(Tr("Remove"))) {
        std::string error;
        if (!RemoveTitleUpdate(app.folder, error)) app.dlc_log.push_back(error);
        RefreshDlc(app);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(Tr("Cancel"))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  } else if (!TitleUpdateBuildPresent(app.folder)) {
    ImGui::TextWrapped("%s", Tr("This version was released without its title-update build, so the update can't be installed. Pick a version that has it on the Builds page."));
  } else {
    ImGui::TextWrapped("%s", Tr("Not installed. The title update adds Expert mode, and Rune Battle and Time Attack in Multiplayer. Copy it from your console's hard drive (Content\\0000000000000000\\4D5307D2\\000B0000) or from Xenia's content folder (the same 4D5307D2\\000B0000), then install it here."));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(Tr("Install Title Update..."))) StartTitleUpdateInstall(app, PickPaths(false, Wide(Tr(u"Choose the Kameo title update package"))));
    ImGui::SameLine();
    if (ImGui::Button(Tr("From Folder..."))) StartTitleUpdateInstall(app, PickPaths(true, Wide(Tr(u"Choose the folder with the Kameo title update"))));
    ImGui::EndDisabled();
  }
  ImGui::EndChild();
}

std::string SizeText(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
  return text;
}

}  // namespace

void PageDlc(App& app) {
  PageTitle(app, Tr("Downloadable Content"), Tr("Install Kameo's Xbox 360 DLC packages so the game can use them."));
  const bool busy = app.installer.running();
  ImGui::BeginChild("dlc_install", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("Install"));
  ImGui::Separator();
  ImGui::TextWrapped("%s", Tr("Choose the folder you downloaded the DLC into, such as the one ending in 4D5307D2\\00000002, or pick the package files themselves. Every Kameo package found is unpacked, the title update included; anything else is skipped."));
  ImGui::BeginDisabled(busy);
  if (ImGui::Button(Tr("Install from Folder..."))) StartInstall(app, PickPaths(true, Wide(Tr(u"Choose the folder with your Kameo DLC"))));
  ImGui::SameLine();
  if (ImGui::Button(Tr("Install Files..."))) StartInstall(app, PickPaths(false, Wide(Tr(u"Choose Kameo DLC package files"))));
  ImGui::EndDisabled();
  if (busy) {
    ImGui::ProgressBar(app.installer.progress(), ImVec2(-FLT_MIN, 0), TrFormat("Unpacking {0}", {app.installer.current()}).c_str());
  }
  for (const std::string& line : app.dlc_log) ImGui::TextColored(kTextDim, "%s", line.c_str());
  ImGui::EndChild();
  ImGui::Spacing();
  TitleUpdateCard(app, busy);
  ImGui::Spacing();

  ImGui::BeginChild("dlc_installed", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "%s", Tr("Installed"));
  ImGui::SameLine();
  const char* open_folder = Tr("Open Folder");
  const float width = ImGui::CalcTextSize(open_folder).x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
  if (ImGui::Button(open_folder)) {
    std::error_code ec;
    const std::filesystem::path folder = app.content_root / "0000000000000000" / "4D5307D2" / "00000002";
    std::filesystem::create_directories(folder, ec);
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::Separator();
  if (app.installed.empty()) {
    ImGui::TextColored(kTextDim, "%s", Tr("Nothing installed yet."));
  } else if (ImGui::BeginTable("dlc_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(Tr("Name"), ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn(Tr("In this version"), ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableSetupColumn(Tr("Size"), ImGuiTableColumnFlags_WidthStretch, 0.13f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.15f);
    ImGui::TableHeadersRow();
    std::string remove;
    for (const InstalledDlc& dlc : app.installed) {
      ImGui::PushID(dlc.file_name.c_str());
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(dlc.display_name.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dlc.file_name.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kTextDim, "%s", dlc.support.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(SizeText(dlc.size_bytes).c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::BeginDisabled(busy);
      if (ImGui::Button(Tr("Remove"))) ImGui::OpenPopup("confirm_remove");
      ImGui::EndDisabled();
      if (ImGui::BeginPopup("confirm_remove")) {
        ImGui::TextUnformatted(TrFormat("Remove {0}?", {dlc.display_name}).c_str());
        if (ImGui::Button(Tr("Remove"))) {
          remove = dlc.file_name;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (!remove.empty()) {
      std::string error;
      if (!RemoveDlc(app.content_root, remove, error)) app.dlc_log.push_back(error);
      RefreshDlc(app);
    }
  }
  ImGui::EndChild();
}

}  // namespace kameo::launcher
