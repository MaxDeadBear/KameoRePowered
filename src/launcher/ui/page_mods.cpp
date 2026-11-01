// Mods page: .kmod level mods in load order, shader mods, texture and movie replacements, and gameplay switches.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h>

#include "core/save_backup.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {
namespace {

void LevelModsSection(App& app) {
  LevelMods& mods = app.level_mods;
  ImGui::BeginChild("level_mods", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Mod Packages");
  const float buttons = ImGui::CalcTextSize("Refresh").x + ImGui::CalcTextSize("Open Folder").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - buttons);
  if (ImGui::Button("Refresh")) mods.Load(app.folder);
  ImGui::SameLine();
  if (ImGui::Button("Open Folder")) ::ShellExecuteW(nullptr, L"open", mods.folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  ImGui::Separator();
  if (mods.mods().empty()) {
    ImGui::TextColored(kTextDim, "No mods yet. Put .kmod files in the mods folder (Open Folder), or make your own with the Blender add-on in the modkit folder.");
  } else if (ImGui::BeginTable("level_mod_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed);
    bool changed = false;
    for (size_t i = 0; i < mods.mods().size(); ++i) {
      LevelMod& mod = mods.mods()[i];
      ImGui::PushID(int(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::BeginDisabled(!mod.error.empty());
      if (Toggle("enabled", mod.enabled)) mod.enabled = !mod.enabled, changed = true;
      ImGui::EndDisabled();
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(mod.name.c_str());
      if (!mod.version.empty() || !mod.author.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kTextDim, "%s%s%s", mod.version.empty() ? "" : ("v" + mod.version).c_str(), mod.version.empty() || mod.author.empty() ? "" : "  ", mod.author.empty() ? "" : ("by " + mod.author).c_str());
      }
      if (!mod.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", mod.error.c_str());
      } else {
        std::string levels;
        for (const std::string& level : mod.levels) levels += (levels.empty() ? "" : ", ") + level;
        ImGui::TextColored(kTextDim, "Changes %s", levels.c_str());
        if (!mod.description.empty()) ImGui::TextWrapped("%s", mod.description.c_str());
      }
      ImGui::TableSetColumnIndex(2);
      if (ImGui::ArrowButton("up", ImGuiDir_Up)) mods.Move(i, -1), changed = true;
      ImGui::SameLine();
      if (ImGui::ArrowButton("down", ImGuiDir_Down)) mods.Move(i, 1), changed = true;
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (changed) mods.Save();
  }
  for (const std::string& conflict : mods.Conflicts()) ImGui::TextColored(kGold, "%s", conflict.c_str());
  ImGui::TextColored(kTextDim, "Enabled mods are applied when you press Play, into a copy of the game folder made of links; the game files themselves are never changed.");
  ImGui::TextColored(kTextDim, "Kameo skins replace her Bonus costumes: pick the costume in the storybook's Bonus pages.");
  ImGui::EndChild();
  ImGui::Spacing();
}

// Shader mods are files the game reads from shader_mods/ by itself, even while it runs; one switched off ends in .off.
void ShaderModsSection(App& app) {
  namespace fs = std::filesystem;
  constexpr double kRescanSeconds = 1.0;
  static std::vector<fs::path> files;
  static double scanned = -kRescanSeconds;
  const fs::path folder = app.folder / "shader_mods";
  if (ImGui::GetTime() - scanned >= kRescanSeconds) {
    scanned = ImGui::GetTime();
    files.clear();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
      const std::string name = entry.path().filename().string();
      if (name.ends_with(".kshader") || name.ends_with(".kshader.off")) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
  }
  const bool has_editor = fs::exists(app.folder / "KameoShaderEditor.exe");
  ImGui::BeginChild("shader_mods", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Shader Mods");
  const float buttons = (has_editor ? ImGui::CalcTextSize("Shader Editor").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x : 0.0f) + ImGui::CalcTextSize("Open Folder").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - buttons);
  if (has_editor) {
    if (ImGui::Button("Shader Editor")) ::ShellExecuteW(nullptr, L"open", (app.folder / "KameoShaderEditor.exe").c_str(), nullptr, app.folder.c_str(), SW_SHOWNORMAL);
    ImGui::SameLine();
  }
  if (ImGui::Button("Open Folder##shader")) {
    std::error_code error;
    fs::create_directories(folder, error);
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::Separator();
  if (files.empty()) ImGui::TextColored(kTextDim, "No shader mods yet. Put .kshader files in the shader_mods folder, or make your own with the Shader Editor.");
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string name = files[i].filename().string();
    const bool on = name.ends_with(".kshader");
    ImGui::PushID(int(i));
    if (Toggle("enabled", on)) {
      fs::path renamed = files[i];
      if (on) renamed += ".off";
      else renamed.replace_extension();
      std::error_code error;
      fs::rename(files[i], renamed, error);
      scanned = -kRescanSeconds;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name.substr(0, name.find(".kshader")).c_str());
    ImGui::PopID();
  }
  ImGui::TextColored(kTextDim, "Shader mods change how things are drawn, and apply straight away, even with the game running.");
  ImGui::EndChild();
  ImGui::Spacing();
}

// Shown once a cheat has been used. The restore takes a second click, since it replaces the player's saves.
void CheatsUsedCard(App& app) {
  static bool confirming = false;
  static std::string error;
  ImGui::BeginChild("mods_cheats_used", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextColored(kGold, "Cheats Were Used");
  ImGui::Separator();
  if (!save_backup::Exists(app.content_root)) {
    ImGui::TextWrapped("Leaderboard scores are off because a cheat was used, and there is no backup of your saves from before it.");
  } else {
    ImGui::TextWrapped("Leaderboard scores are off because a cheat was used. Your saves were backed up just before that. Restoring them turns the cheats off and lets you post scores again, but any progress since then goes back too. Your current saves are kept in the save_backups folder.");
    if (!confirming) {
      if (ImGui::Button("Restore Saves From Before Cheats")) confirming = true;
    } else {
      if (ImGui::Button("Yes, Restore My Saves")) {
        confirming = false;
        error.clear();
        if (RestoreCheatSaves(app, error)) app.status = "Your saves from before cheats are back.";
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) confirming = false;
    }
    if (!error.empty()) ImGui::TextColored(kGold, "%s", error.c_str());
  }
  ImGui::EndChild();
  ImGui::Spacing();
}

}  // namespace

void PageMods(App& app) {
  PageTitle(app, "Mods", nullptr);
  if (app.settings.GetBool("kameo_cheats_used")) CheatsUsedCard(app);
  LevelModsSection(app);
  ShaderModsSection(app);
  RestoreButton(app, {"kameo_gfx_texture_replacements", "kameo_gfx_dump_textures", "kameo_movie_replacements","kameo_story_splitscreen", "kameo_unlock_all_extras", "kameo_skip_cutscenes"});
  if (BeginSection("textures", "Textures")) {
    BoolRow(app, "Texture Replacements", "kameo_gfx_texture_replacements", "Load replacement textures from the textures folder.");
    BoolRow(app, "Texture Dumping", "kameo_gfx_dump_textures", "Save the game's textures as they load, for making replacements.");
    BoolRow(app, "Movie Replacements", "kameo_movie_replacements", "Play movies/<name>.mp4 instead of fmv/<name>.bik, e.g. movies/kameoIntro.mp4. With a soundtrack it plays its own sound for its own length; without one only the picture is replaced.");
  }
  EndSection();
  if (BeginSection("gameplay", "Gameplay")) {
    BoolRow(app, "Story Split-screen", "kameo_story_splitscreen");
    BoolRow(app, "Unlock All Extras", "kameo_unlock_all_extras");
    BoolRow(app, "Skippable Cutscenes", "kameo_skip_cutscenes", "Start skips in-engine cutscenes and every FMV.");
  }
  EndSection();
}

}  // namespace kameo::launcher
