// Kameo RePowered launcher: the state every page shares, its palette, and the window, launch and preference entry points.
#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher/builds.h"
#include "launcher/dlc_install.h"
#include "launcher/level_mods.h"
#include "launcher/settings_file.h"

namespace kameo::launcher {

// Deep indigo grounds with the gold of Kameo's own menus and the blue of her elemental glow.
inline constexpr ImU32 kGroundTop = IM_COL32(28, 17, 62, 255);
inline constexpr ImU32 kGroundBottom = IM_COL32(6, 8, 24, 255);
inline constexpr ImVec4 kGold = ImVec4(0.96f, 0.76f, 0.36f, 1.0f);
inline constexpr ImVec4 kText = ImVec4(0.93f, 0.91f, 0.98f, 1.0f);
inline constexpr ImVec4 kTextDim = ImVec4(0.66f, 0.62f, 0.80f, 1.0f);

enum class Page { Play, Video, Audio, Controls, Bindings, Mods, Dlc, Builds };

struct App {
  std::filesystem::path folder;
  Settings settings{std::filesystem::path()};
  std::filesystem::path content_root;
  std::vector<InstalledDlc> installed;
  DlcInstaller installer;
  LevelMods level_mods;
  std::vector<std::string> dlc_log;
  Page page = Page::Play;
  bool close_on_launch = true;
  // The game builds from the GitHub releases: the one in the game folder, whether update notices include pre-releases, and whether to look for builds on start.
  BuildManager builds;
  std::string active_build;
  bool include_prereleases = false;
  bool check_for_builds = true;
  std::string status;
  std::string capture_target;
  std::chrono::steady_clock::time_point capture_started;
  ImFont* body = nullptr;
  ImFont* heading = nullptr;
  float dpi_scale = 1.0f;
  bool quit = false;
};

// Opens the launcher window and runs it until it closes; false when Direct3D 11 could not be started.
bool RunLauncherWindow(HINSTANCE instance, int show, App& app);
void DrawFrame(App& app);

void RefreshDlc(App& app);
bool LaunchGame(App& app);
void LoadPrefs(App& app);
void SavePrefs(const App& app);

// Finishes a key binding started on the Key Bindings page once a key is pressed or it times out.
void PollCapture(App& app);

}  // namespace kameo::launcher
