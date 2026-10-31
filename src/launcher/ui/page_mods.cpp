// Mods page: .kmod level mods in load order, texture and movie replacements, and gameplay switches.

#include <string>

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
