// The system cursor: shown in menus and hidden over the window in play, and drawn as the Kameo arrow on Windows.

#include <atomic>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/logging/log_category.h>
#include <rex/module.h>
#include <rex/ui/ui_system.h>

#include "cursor/cursor_images.inc"
#include "input/input_internal.h"

namespace kameo::input::detail {

namespace {

// Whether the pointer is shown over the window (not hidden or held for looking).
std::atomic<bool> g_cursorShown{false};
std::atomic<bool> g_cursorRefreshQueued{false};

#ifdef _WIN32
// The Kameo arrow from res/cursor/kameo_cursor.webp (tools/build_cursor.py), shown by answering the window's WM_SETCURSOR wherever the runtime would show its own arrow.
constexpr int kCursorSizeAt96Dpi = 40;
HWND g_cursorWindow = nullptr;
WNDPROC g_cursorPreviousProc = nullptr;
HCURSOR g_kameoCursor = nullptr;

HCURSOR CreateKameoCursor(UINT dpi) {
  const int wanted = MulDiv(kCursorSizeAt96Dpi, int(dpi ? dpi : 96), 96);
  const KameoCursorImage* image = &kKameoCursorImages[0];
  for (const KameoCursorImage& candidate : kKameoCursorImages) {
    if (std::abs(candidate.size - wanted) < std::abs(image->size - wanted)) image = &candidate;
  }
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = image->size;
  header.bV5Height = -image->size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HBITMAP colour = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  HBITMAP mask = ::CreateBitmap(image->size, image->size, 1, 1, nullptr);
  HCURSOR cursor = nullptr;
  if (colour && mask && bits) {
    std::memcpy(bits, image->bgra, size_t(image->size) * size_t(image->size) * 4);
    ICONINFO info = {FALSE, DWORD(image->hot_x), DWORD(image->hot_y), mask, colour};
    cursor = reinterpret_cast<HCURSOR>(::CreateIconIndirect(&info));
  }
  if (colour) ::DeleteObject(colour);
  if (mask) ::DeleteObject(mask);
  return cursor;
}

bool KameoCursorWanted() { return g_kameoCursor && g_cursorShown.load(std::memory_order_relaxed); }

LRESULT CALLBACK KameoCursorWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && KameoCursorWanted()) {
    ::SetCursor(g_kameoCursor);
    return TRUE;
  }
  if (message == WM_DPICHANGED) {
    if (HCURSOR resized = CreateKameoCursor(HIWORD(wparam))) {
      HCURSOR previous = g_kameoCursor;
      g_kameoCursor = resized;
      if (::GetCursor() == previous) ::SetCursor(resized);
      if (previous) ::DestroyCursor(previous);
    }
  }
  return ::CallWindowProcW(g_cursorPreviousProc, hwnd, message, wparam, lparam);
}

// Showing the pointer puts the runtime's arrow up straight away, so the Kameo one replaces it without waiting for the mouse to move.
void RefreshKameoCursor() {
  if (!g_cursorWindow || !KameoCursorWanted()) return;
  POINT point = {};
  if (!::GetCursorPos(&point) || ::WindowFromPoint(point) != g_cursorWindow) return;
  ::SendMessageW(g_cursorWindow, WM_SETCURSOR, WPARAM(g_cursorWindow), MAKELPARAM(::SendMessageW(g_cursorWindow, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)), WM_MOUSEMOVE));
}
#endif

}  // namespace

#ifdef _WIN32
void InstallKameoCursor() {
  rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem);
  if (!ui) return;
  ui->PostTask([ui] {
    const auto native = ui->GetNativeWindow();
    HWND hwnd = native ? static_cast<HWND>(native->window) : nullptr;
    if (!hwnd || g_cursorWindow) return;
    g_kameoCursor = CreateKameoCursor(::GetDpiForWindow(hwnd));
    if (!g_kameoCursor) {
      REXLOG_WARN("[kameo] input: the custom mouse cursor could not be created");
      return;
    }
    g_cursorWindow = hwnd;
    g_cursorPreviousProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&KameoCursorWindowProc)));
    RefreshKameoCursor();
  });
}
#endif

// Re-asserted every tick because the runtime applies the mode it was last given again when the window regains focus.
void ApplyPointerMode(bool captured, bool menu_open) {
  rex::hid::IInputSystem* input = InputSystem();
  if (!input) return;
  const rex::PointerMode mode = captured ? rex::PointerMode::kLocked : menu_open ? rex::PointerMode::kFree : rex::PointerMode::kHidden;
  input->SetPointerMode(mode);
  const bool shown = mode == rex::PointerMode::kFree;
  if (g_cursorShown.exchange(shown) == shown) return;
#ifdef _WIN32
  rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem);
  if (!ui || g_cursorRefreshQueued.exchange(true)) return;
  ui->PostTask([] {
    g_cursorRefreshQueued.store(false);
    RefreshKameoCursor();
  });
#endif
}

}  // namespace kameo::input::detail
