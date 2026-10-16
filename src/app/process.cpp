// Restarting and quitting the game from its menus, and a restarted copy waiting for the one it replaces.

#include "app/process.h"

#include <atomic>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_UINT32(kameo_restart_wait_pid, 0, "Kameo", "Set by the in-game restart: the process id of the copy being replaced, which this launch waits to exit before starting.");

namespace {

std::atomic<rex::ui::Window*> g_window{nullptr};
// Set once a restart or quit is under way, so a second press while the window closes neither launches another copy nor turns a quit into a restart.
bool g_closing = false;

}  // namespace

void kameo::app::SetMainWindow(rex::ui::Window* window) { g_window.store(window); }

rex::ui::Window* kameo::app::MainWindow() { return g_window.load(); }

void KameoWaitForReplacedProcess() {
#ifdef _WIN32
  const uint32_t pid = REXCVAR_GET(kameo_restart_wait_pid);
  if (!pid) return;
  if (HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid)) {
    // The old copy still holds the audio device and the game's files until it exits.
    if (::WaitForSingleObject(process, 15000) == WAIT_TIMEOUT) REXLOG_WARN("[kameo] restart: previous process {} still running after 15 s; starting anyway", pid);
    ::CloseHandle(process);
  }
#endif
}

void KameoQuitApplication() {
  rex::ui::Window* window = g_window.load();
  if (!window || g_closing) return;
  g_closing = true;
  REXLOG_INFO("[kameo] quit: closing the game from the pause menu");
  window->app_context().CallInUIThreadDeferred([window] { window->RequestClose(); });
}

void KameoRestartApplication() {
#ifdef _WIN32
  if (g_closing) return;
  wchar_t exe[MAX_PATH];
  if (!::GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
  // Keep the original arguments, minus the executable and any wait flag an earlier restart added.
  std::wstring line = ::GetCommandLineW();
  size_t args = 0;
  if (!line.empty() && line[0] == L'"') {
    args = line.find(L'"', 1);
    args = args == std::wstring::npos ? line.size() : args + 1;
  } else {
    args = line.find(L' ');
    if (args == std::wstring::npos) args = line.size();
  }
  std::wstring rest = line.substr(args);
  for (size_t at = rest.find(L" --kameo_restart_wait_pid="); at != std::wstring::npos; at = rest.find(L" --kameo_restart_wait_pid=")) {
    size_t end = rest.find(L' ', at + 1);
    rest.erase(at, end == std::wstring::npos ? std::wstring::npos : end - at);
  }
  std::wstring command = L"\"" + std::wstring(exe) + L"\"" + rest + L" --kameo_restart_wait_pid=" + std::to_wstring(::GetCurrentProcessId());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!::CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    REXLOG_ERROR("[kameo] restart: could not launch a new copy (error {})", ::GetLastError());
    return;
  }
  g_closing = true;
  ::CloseHandle(info.hThread);
  ::CloseHandle(info.hProcess);
  REXLOG_INFO("[kameo] restart: launched a new copy, closing this one");
  if (rex::ui::Window* window = g_window.load()) {
    window->app_context().CallInUIThreadDeferred([window] { window->RequestClose(); });
  }
#endif
}
