// The game window and the process around it: restarting, quitting and recovering from a crashed fullscreen session.

#pragma once

namespace rex::ui { class Window; }

// Relaunches the game with the same arguments and closes this copy; the new one waits for this process to exit first.
void KameoRestartApplication();
// Closes the game the way the window's close button does.
void KameoQuitApplication();
void KameoWaitForReplacedProcess();

namespace kameo::app {

// The runtime's game window, once the app has created it.
void SetMainWindow(rex::ui::Window* window);
rex::ui::Window* MainWindow();

// Called while settings load: a marker left by a session that crashed in exclusive fullscreen switches the saved mode to borderless.
void RecoverFromCrashedFullscreen();

}  // namespace kameo::app
