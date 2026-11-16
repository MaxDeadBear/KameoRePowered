// The launcher's own copy of the runtime libraries.
//
// The launcher takes ImGui, the module loader and the host allocator from rexbase.dll, and the package and disc image readers from the rexfilesystem module that rexbase loads beside itself, but switching builds replaces the game's copies next to the launcher, so the launcher must neither lock those files nor depend on whichever version they are.
// rexbase.dll is delay-loaded (see CMakeLists.txt), and this hook loads launcher\rexbase.dll, the copy built with the launcher, in its place; rexfilesystem.dll sits beside it there. Without that folder (a development build) it falls back to the ones beside the game.
// The file name carries the build configuration's postfix (rexbased.dll, rexbaserd.dll), so any name that starts with rexbase is redirected under that name.
// The hook runs while rexbase is being loaded, and operator new is rexbase's own, so it must not allocate: it works in fixed buffers.

#include <windows.h>

#include <delayimp.h>

#include <cstring>

namespace {

FARPROC WINAPI LoadLauncherRuntime(unsigned notification, PDelayLoadInfo info) {
  if (notification != dliNotePreLoadLibrary || !info || !info->szDll || _strnicmp(info->szDll, "rexbase", 7) != 0) return nullptr;
  wchar_t path[MAX_PATH];
  const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return nullptr;
  DWORD folder = length;
  while (folder > 0 && path[folder - 1] != L'\\' && path[folder - 1] != L'/') --folder;
  constexpr wchar_t kSubfolder[] = L"launcher\\";
  const size_t name_length = std::strlen(info->szDll);
  if (folder + (sizeof(kSubfolder) / sizeof(wchar_t) - 1) + name_length + 1 > MAX_PATH) return nullptr;
  wchar_t* out = path + folder;
  for (const wchar_t c : kSubfolder) {
    if (c) *out++ = c;
  }
  for (size_t i = 0; i < name_length; ++i) *out++ = wchar_t(static_cast<unsigned char>(info->szDll[i]));
  *out = L'\0';
  if (::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
  // The altered search path lets the copy find its own dependencies beside it first.
  return reinterpret_cast<FARPROC>(::LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = LoadLauncherRuntime;
