// The themed Direct3D 11 ImGui window the launcher and the crash dialog both run in.
#pragma once

#include <windows.h>

#include <functional>

struct ImFont;

namespace kameo::launcher {

struct WindowOptions {
  const wchar_t* title;
  int width;
  int height;
  // Zero leaves the window free to shrink.
  int min_width = 0;
  int min_height = 0;
  // A fixed-size window that opens on top of everything, for a question that has to be answered.
  bool dialog = false;
};

// What the window shares with what is drawn in it.
struct WindowState {
  ImFont* body = nullptr;
  ImFont* heading = nullptr;
  float dpi_scale = 1.0f;
  bool quit = false;
};

// Opens the window and calls draw once a frame until it closes or state.quit is set; false when Direct3D 11 could not be started.
bool RunWindow(HINSTANCE instance, int show, const WindowOptions& options, WindowState& state, const std::function<void()>& draw);

}  // namespace kameo::launcher
