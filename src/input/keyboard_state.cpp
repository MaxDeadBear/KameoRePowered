// Shift and Caps Lock read from the OS, since ImGui's shift modifier never reaches the name entry's key feed.

#ifdef _WIN32
#include <windows.h>
#endif

#include "input/input.h"

bool KameoKeyboardShiftHeld() {
#ifdef _WIN32
  return (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  return false;
#endif
}

bool KameoKeyboardCapitals() {
#ifdef _WIN32
  return KameoKeyboardShiftHeld() != ((::GetKeyState(VK_CAPITAL) & 1) != 0);
#else
  return false;
#endif
}
