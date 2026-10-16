#ifdef _WIN32

#include <windows.h>

#include "app/app.h"

namespace kameo::app {

void DeclarePerMonitorDpiAwareness() {
  HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
  if (!user32) return;
  using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  if (auto set_context = reinterpret_cast<SetContextFn>(reinterpret_cast<void*>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")))) {
    if (set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    if (set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return;
  }
  using SetAwareFn = BOOL(WINAPI*)();
  if (auto set_aware = reinterpret_cast<SetAwareFn>(reinterpret_cast<void*>(::GetProcAddress(user32, "SetProcessDPIAware")))) {
    set_aware();
  }
}

}  // namespace kameo::app

#endif
