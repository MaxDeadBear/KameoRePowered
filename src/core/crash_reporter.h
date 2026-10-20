// Crash reporter: when the process dies of an unhandled exception, the crash's code, address and stack go into the log and a crash_<time>.txt and .dmp beside it, as module+offset pairs the build's PDB turns into function names.
#pragma once

// Installs the process-wide last-chance exception filter; call once logging exists. Windows only: elsewhere it does nothing.
void InstallCrashReporter();

// Writes through a null pointer when kameo_crash_test is set, so the report can be checked without a real crash.
void CrashForTestIfAsked();
