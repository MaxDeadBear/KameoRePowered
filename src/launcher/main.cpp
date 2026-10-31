// Kameo RePowered launcher: every setting the pause menu has, key bindings and DLC installs for the game beside it, and a Play button that starts it.

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui_backends/imgui_impl_win32.h"
#include "launcher/app.h"

namespace {

// The console modes report to the console they were started from, or to wherever their output was redirected.
void UseConsole() {
  const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  const DWORD type = out && out != INVALID_HANDLE_VALUE ? ::GetFileType(out) : FILE_TYPE_UNKNOWN;
  if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;
  FILE* reopened = nullptr;
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) freopen_s(&reopened, "CONOUT$", "w", stdout);
}

bool OpenLauncherWindow(HINSTANCE instance, int show, kameo::launcher::App& app) {
  using namespace kameo::launcher;
  const WindowOptions options = {.title = L"Kameo RePowered Launcher", .width = 1120, .height = 740, .min_width = 900, .min_height = 620};
  if (RunWindow(instance, show, options, app, [&app] { DrawFrame(app); })) return true;
  ::MessageBoxW(nullptr, L"Direct3D 11 could not be started.", L"Kameo RePowered Launcher", MB_ICONERROR);
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  using namespace kameo::launcher;
  ImGui_ImplWin32_EnableDpiAwareness();
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  App app;
  wchar_t module[MAX_PATH];
  ::GetModuleFileNameW(nullptr, module, MAX_PATH);
  app.folder = std::filesystem::path(module).parent_path();

  // KameoLauncher --install-dlc <folder or file> [--content-root <folder>] installs without opening the window and reports to the console it was started from.
  int argument_count = 0;
  LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argument_count);
  std::filesystem::path install_source;
  std::filesystem::path content_root_override;
  bool build_mods = false;
  bool list_builds = false;
  std::string switch_build;
  std::filesystem::path unpack_source;
  bool restore_saves = false;
  for (int i = 1; arguments && i < argument_count; ++i) {
    if (std::wstring(arguments[i]) == L"--build-mods") build_mods = true;
    else if (std::wstring(arguments[i]) == L"--restore-cheat-saves") restore_saves = true;
    else if (std::wstring(arguments[i]) == L"--list-builds") list_builds = true;
    else if (i + 1 < argument_count && std::wstring(arguments[i]) == L"--page") {
      // Opens on a page by name, for shortcuts and screenshots.
      const std::wstring page = arguments[++i];
      if (page == L"builds") app.page = Page::Builds;
      else if (page == L"dlc") app.page = Page::Dlc;
      else if (page == L"mods") app.page = Page::Mods;
    }
    else if (i + 1 < argument_count && std::wstring(arguments[i]) == L"--switch-build") switch_build = std::filesystem::path(arguments[++i]).string();
    else if (i + 1 < argument_count && std::wstring(arguments[i]) == L"--install-dlc") install_source = arguments[++i];
    else if (i + 1 < argument_count && std::wstring(arguments[i]) == L"--unpack-game") unpack_source = arguments[++i];
    else if (i + 1 < argument_count && std::wstring(arguments[i]) == L"--content-root") content_root_override = arguments[++i];
  }
  if (arguments) ::LocalFree(arguments);
  // KameoLauncher --list-builds prints the published builds and which are downloaded; --switch-build <tag> downloads one if needed and switches the game folder to it.
  if (list_builds || !switch_build.empty()) {
    UseConsole();
    LoadPrefs(app);
    app.builds.Init(app.folder, app.active_build);
    auto wait = [&app]() {
      while (app.builds.busy()) ::Sleep(50);
      const std::string message = app.builds.TakeMessage();
      if (!message.empty()) std::printf("%s\n", message.c_str());
    };
    app.builds.Refresh();
    wait();
    int result = app.builds.fetched() ? 0 : 1;
    if (!switch_build.empty() && result == 0) {
      app.builds.Activate(switch_build);
      wait();
      result = app.builds.active() == switch_build ? 0 : 1;
    }
    if (app.builds.TakeActiveChanged()) {
      app.active_build = app.builds.active();
      SavePrefs(app);
    }
    const std::string active = app.builds.active();
    std::printf("In use: %s\n", active.empty() ? "(installed by hand, version unknown)" : active.c_str());
    const auto installed = app.builds.installed();
    for (const BuildRelease& release : app.builds.releases()) {
      bool here = false;
      for (const InstalledBuild& build : installed) here = here || build.tag == release.tag;
      std::printf("  %-22s %s %-11s %s\n", release.tag.c_str(), release.published.c_str(), release.prerelease ? "pre-release" : "release", release.tag == active ? "in use" : here ? "downloaded" : release.asset_url.empty() ? "no Windows build" : "");
    }
    for (const InstalledBuild& build : installed) {
      if (build.saved_local) std::printf("  %-22s (your previous build)%s\n", build.tag.c_str(), build.tag == active ? " in use" : "");
    }
    std::fflush(stdout);
    ::CoUninitialize();
    return result;
  }
  // KameoLauncher --build-mods prepares the data folder for the enabled level mods and prints the arguments the game would get.
  if (build_mods) {
    UseConsole();
    app.level_mods.Load(app.folder);
    std::filesystem::path data_root;
    std::string error;
    int result = 0;
    if (!app.level_mods.AnyEnabled()) std::printf("No level mods are enabled.\n");
    else if (app.level_mods.Build(data_root, error)) std::printf("--game_data_root=\"%s\"\n", data_root.string().c_str());
    else std::printf("%s\n", error.c_str()), result = 1;
    for (const std::string& conflict : app.level_mods.Conflicts()) std::printf("%s\n", conflict.c_str());
    std::fflush(stdout);
    ::CoUninitialize();
    return result;
  }
  // KameoLauncher --unpack-game <disc image or Games on Demand package> unpacks it into assets without opening the window.
  if (!unpack_source.empty()) {
    UseConsole();
    app.iso.Start(unpack_source, app.folder / "assets");
    while (app.iso.running()) ::Sleep(50);
    std::printf("%s\n", app.iso.TakeResult().c_str());
    std::fflush(stdout);
    const int result = app.iso.installed() ? 0 : 1;
    ::CoUninitialize();
    return result;
  }
  if (!install_source.empty()) {
    UseConsole();
    const std::filesystem::path root = content_root_override.empty() ? ContentRoot(app.folder) : content_root_override;
    const std::vector<std::filesystem::path> packages = FindPackages(install_source);
    std::printf("Installing %d package(s) into %s\n", int(packages.size()), root.string().c_str());
    app.installer.Start(packages, root);
    while (app.installer.running()) ::Sleep(50);
    int failures = 0;
    for (const std::string& line : app.installer.TakeResults()) {
      std::printf("%s\n", line.c_str());
      if (line.rfind("Installed ", 0) != 0) ++failures;
    }
    std::fflush(stdout);
    ::CoUninitialize();
    return packages.empty() ? 2 : failures;
  }
  app.settings = Settings(app.folder / "kameo_settings.toml");
  app.settings.Load();
  app.content_root = ContentRoot(app.folder);
  // KameoLauncher --restore-cheat-saves does what the Mods page's button does.
  if (restore_saves) {
    UseConsole();
    std::string error;
    const bool restored = RestoreCheatSaves(app, error);
    std::printf("%s\n", restored ? "Your saves from before cheats are back." : error.c_str());
    std::fflush(stdout);
    ::CoUninitialize();
    return restored ? 0 : 1;
  }
  app.level_mods.Load(app.folder);
  app.has_game_files = HasGameFiles(app.folder);
  LoadPrefs(app);
  RefreshDlc(app);
  app.builds.Init(app.folder, app.active_build);
  if (app.check_for_builds) app.builds.Refresh();

  if (!OpenLauncherWindow(instance, show, app)) return 1;
  ::CoUninitialize();
  return 0;
}
