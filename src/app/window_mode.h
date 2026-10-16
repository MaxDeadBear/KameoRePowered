// The game window's mode: windowed, borderless or exclusive fullscreen, saved as kameo_window_mode.
#pragma once

#include <cstdint>

namespace rex::ui { class Window; }

// 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen; applied on the UI thread.
int32_t KameoWindowMode();
void KameoSetWindowMode(int32_t mode);
void KameoApplyWindowMode();
void KameoInitWindowMode(rex::ui::Window* window);
// Marks a session running in exclusive fullscreen, so one that crashes starts borderless next time.
void KameoNoteExclusiveFullscreen(bool active);
