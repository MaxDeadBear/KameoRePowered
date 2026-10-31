// Crash reporter: when the process dies of an unhandled exception, the crash's code, address and stack go into the log and a crash_<time>.txt and .dmp beside it, as module+offset pairs the build's PDB turns into function names.
#pragma once

// Installs the process-wide last-chance exception filter and abort handler; call once logging exists. Calling it again puts it back on top of a filter installed since, which it then hands the crash on to. When Sentry is on, that happens only if the player agrees in KameoCrashDialog.exe. Windows only: elsewhere it does nothing.
void InstallCrashReporter();

// Writes through a null pointer when kameo_crash_test is set, so the report can be checked without a real crash.
void CrashForTestIfAsked();

// The same during play, once a frame: crashes while Ctrl+Shift+F12 is held in the game, when kameo_crash_key is set.
void CrashIfTestKeyHeld();
