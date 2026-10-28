// The launcher's own copy of the runtime DLL.
//
// The launcher takes ImGui and the STFS reader from rexruntime.dll, but switching builds replaces the game's rexruntime.dll next to it, so the launcher must neither lock that file nor depend on whichever version it is.
// rexruntime.dll is delay-loaded (see CMakeLists.txt), and this hook loads launcher\rexruntime.dll, the copy built with the launcher, in its place. Without that folder (a development build) it falls back to the one beside the game.

#include <windows.h>

#include <delayimp.h>

#include <cwchar>
#include <string>

namespace {

FARPROC WINAPI LoadLauncherRuntime(unsigned notification, PDelayLoadInfo info) {
  if (notification != dliNotePreLoadLibrary || !info || !info->szDll || _stricmp(info->szDll, "rexruntime.dll") != 0) return nullptr;
  wchar_t module[MAX_PATH];
  const DWORD length = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return nullptr;
  std::wstring path(module, length);
  path.resize(path.find_last_of(L"\\/") + 1);
  path += L"launcher\\rexruntime.dll";
  if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
  // The altered search path lets the copy find its own dependencies beside it first.
  return reinterpret_cast<FARPROC>(::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = LoadLauncherRuntime;
