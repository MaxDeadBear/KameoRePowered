// DLC page: installing Xbox 360 DLC packages from a folder or files, and removing installed ones.

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h>
#include <shobjidl.h>

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {
namespace {

std::vector<std::filesystem::path> PickPaths(bool folders) {
  std::vector<std::filesystem::path> picked;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return picked;
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT));
  dialog->SetTitle(folders ? L"Choose the folder with your Kameo DLC" : L"Choose Kameo DLC package files");
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
    app.dlc_log.push_back("No Xbox 360 packages were found there. Pick the folder holding the DLC files (for example ...\\4D5307D2\\00000002).");
    return;
  }
  app.installer.Start(std::move(packages), app.content_root);
}

std::string SizeText(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
  return text;
}

}  // namespace

void PageDlc(App& app) {
  PageTitle(app, "Downloadable Content", "Install Kameo's Xbox 360 DLC packages so the game can use them.");
  const bool busy = app.installer.running();
  ImGui::BeginChild("dlc_install", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Install");
  ImGui::Separator();
  ImGui::TextWrapped("Choose the folder you downloaded the DLC into, such as the one ending in 4D5307D2\\00000002, or pick the package files themselves. Every Kameo package found is unpacked; anything else is skipped.");
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Install from Folder...")) StartInstall(app, PickPaths(true));
  ImGui::SameLine();
  if (ImGui::Button("Install Files...")) StartInstall(app, PickPaths(false));
  ImGui::EndDisabled();
  if (busy) {
    ImGui::ProgressBar(app.installer.progress(), ImVec2(-FLT_MIN, 0), ("Unpacking " + app.installer.current()).c_str());
  }
  for (const std::string& line : app.dlc_log) ImGui::TextColored(kTextDim, "%s", line.c_str());
  ImGui::EndChild();
  ImGui::Spacing();

  ImGui::BeginChild("dlc_installed", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Installed");
  ImGui::SameLine();
  const float width = ImGui::CalcTextSize("Open Folder").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
  if (ImGui::Button("Open Folder")) {
    std::error_code ec;
    const std::filesystem::path folder = app.content_root / "0000000000000000" / "4D5307D2" / "00000002";
    std::filesystem::create_directories(folder, ec);
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::Separator();
  if (app.installed.empty()) {
    ImGui::TextColored(kTextDim, "Nothing installed yet.");
  } else if (ImGui::BeginTable("dlc_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("In this version", ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.13f);
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
      if (ImGui::Button("Remove")) ImGui::OpenPopup("confirm_remove");
      ImGui::EndDisabled();
      if (ImGui::BeginPopup("confirm_remove")) {
        ImGui::Text("Remove %s?", dlc.display_name.c_str());
        if (ImGui::Button("Remove")) {
          remove = dlc.file_name;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
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
