// Starting the game, with the level mods' data folder when any are enabled, and the launcher's own preferences file.

#include <windows.h>

#include <tlhelp32.h>

#include <fstream>
#include <string>

#include "core/save_backup.h"
#include "launcher/app.h"

namespace kameo::launcher {
namespace {

std::filesystem::path LauncherPrefsPath(const App& app) { return app.folder / "kameo_launcher.toml"; }

bool GameIsRunning() {
  const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry = {sizeof(entry)};
  bool running = false;
  for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !running; more = ::Process32NextW(snapshot, &entry)) {
    running = _wcsicmp(entry.szExeFile, L"kameorepowered.exe") == 0;
  }
  ::CloseHandle(snapshot);
  return running;
}

}  // namespace

void RefreshDlc(App& app) { app.installed = ListInstalledDlc(app.content_root); }

bool RestoreCheatSaves(App& app, std::string& error) {
  // The running game holds its save open and would write its cheated state straight back.
  if (GameIsRunning()) return error = "Close the game first.", false;
  if (!save_backup::Restore(app.content_root, error)) return false;
  app.settings.SetBool("kameo_cheats_used", false);
  app.settings.SetBool("kameo_unlock_all_extras", false);
  app.settings.SetBool("kameo_unlock_all_warriors", false);
  return true;
}

bool LaunchGame(App& app) {
  const std::filesystem::path exe = app.folder / L"kameorepowered.exe";
  std::error_code ec;
  if (!std::filesystem::exists(exe, ec)) {
    app.status = "kameorepowered.exe was not found next to the launcher.";
    return false;
  }
  if (!app.has_game_files) {
    app.status = "The game files are missing. Choose your game on the Play page first.";
    app.page = Page::Play;
    return false;
  }
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  if (app.level_mods.AnyEnabled()) {
    std::filesystem::path data_root;
    std::string error;
    if (!app.level_mods.Build(data_root, error)) {
      app.status = "The level mods could not be applied: " + error;
      return false;
    }
    command += L" --game_data_root=\"" + data_root.wstring() + L"\"";
  }
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, app.folder.c_str(), &startup, &process)) {
    app.status = "The game could not be started (error " + std::to_string(::GetLastError()) + ").";
    return false;
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  app.status = "Kameo is starting...";
  return true;
}

void LoadPrefs(App& app) {
  std::ifstream file(LauncherPrefsPath(app));
  std::string line;
  // One "name = value" per line; strings are quoted.
  auto value = [&line]() {
    const size_t equals = line.find('=');
    const std::string text = equals == std::string::npos ? std::string() : line.substr(equals + 1);
    const size_t first = text.find_first_not_of(" \t\"");
    const size_t last = text.find_last_not_of(" \t\"\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
  };
  while (std::getline(file, line)) {
    if (line.rfind("close_on_launch", 0) == 0) app.close_on_launch = value() != "false";
    else if (line.rfind("active_build", 0) == 0) app.active_build = value();
    else if (line.rfind("include_prereleases", 0) == 0) app.include_prereleases = value() == "true";
    else if (line.rfind("check_for_builds", 0) == 0) app.check_for_builds = value() != "false";
  }
}

void SavePrefs(const App& app) {
  std::ofstream file(LauncherPrefsPath(app), std::ios::trunc);
  file << "# Kameo RePowered launcher preferences.\n";
  file << "close_on_launch = " << (app.close_on_launch ? "true" : "false") << "\n";
  file << "active_build = \"" << app.active_build << "\"\n";
  file << "include_prereleases = " << (app.include_prereleases ? "true" : "false") << "\n";
  file << "check_for_builds = " << (app.check_for_builds ? "true" : "false") << "\n";
}

}  // namespace kameo::launcher
