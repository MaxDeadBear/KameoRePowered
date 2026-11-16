// The process around the game: restarting, quitting and recovering from a crashed fullscreen session.

#pragma once

// Closes this copy and starts the game again with the same arguments.
void KameoRestartApplication();
// Closes the game the way the window's close button does.
void KameoQuitApplication();

namespace kameo::app {

// Called while settings load: a marker left by a session that crashed in exclusive fullscreen switches the saved mode to borderless.
void RecoverFromCrashedFullscreen();

}  // namespace kameo::app
