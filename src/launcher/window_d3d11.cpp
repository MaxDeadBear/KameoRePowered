// The launcher's Win32 window and its Direct3D 11 swap chain, driving one ImGui frame per loop.

#include <d3d11.h>
#include <dwmapi.h>

#include "imgui_backends/imgui_impl_dx11.h"
#include "imgui_backends/imgui_impl_win32.h"
#include "launcher/ui/widgets.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace kameo::launcher {
namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
UINT g_resize_width = 0;
UINT g_resize_height = 0;
App* g_app = nullptr;

void CreateTarget() {
  ID3D11Texture2D* back_buffer = nullptr;
  g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  g_device->CreateRenderTargetView(back_buffer, nullptr, &g_target);
  back_buffer->Release();
}

void ReleaseTarget() {
  if (g_target) {
    g_target->Release();
    g_target = nullptr;
  }
}

bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferCount = 2;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferDesc.RefreshRate.Numerator = 60;
  desc.BufferDesc.RefreshRate.Denominator = 1;
  desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = hwnd;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL level;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &g_swap_chain, &g_device, &level, &g_context);
  if (hr == DXGI_ERROR_UNSUPPORTED) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &g_swap_chain, &g_device, &level, &g_context);
  if (FAILED(hr)) return false;
  CreateTarget();
  return true;
}

void ReleaseDevice() {
  ReleaseTarget();
  if (g_swap_chain) g_swap_chain->Release();
  if (g_context) g_context->Release();
  if (g_device) g_device->Release();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) return true;
  switch (message) {
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        g_resize_width = LOWORD(lparam);
        g_resize_height = HIWORD(lparam);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      const float scale = g_app ? g_app->dpi_scale : 1.0f;
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = LONG(900 * scale);
      info->ptMinTrackSize.y = LONG(620 * scale);
      return 0;
    }
    case WM_DPICHANGED: {
      if (g_app) {
        g_app->dpi_scale = float(HIWORD(wparam)) / 96.0f;
        if (ImGui::GetCurrentContext()) ApplyTheme(g_app->dpi_scale);
      }
      const RECT* suggested = reinterpret_cast<RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

bool RunLauncherWindow(HINSTANCE instance, int show, App& app) {
  g_app = &app;
  WNDCLASSEXW window_class = {sizeof(window_class), CS_CLASSDC, WindowProc, 0, 0, instance, ::LoadIconW(instance, L"MAINICON"), ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), nullptr, nullptr, L"KameoLauncher", ::LoadIconW(instance, L"MAINICON")};
  ::RegisterClassExW(&window_class);
  const POINT origin = {0, 0};
  const HMONITOR monitor = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  app.dpi_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
  const int width = int(1120 * app.dpi_scale);
  const int height = int(740 * app.dpi_scale);
  MONITORINFO info = {sizeof(info)};
  ::GetMonitorInfoW(monitor, &info);
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
  const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;
  HWND hwnd = ::CreateWindowW(window_class.lpszClassName, L"Kameo RePowered Launcher", WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, instance, nullptr);
  const BOOL dark = TRUE;
  ::DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

  if (!CreateDevice(hwnd)) {
    ReleaseDevice();
    ::MessageBoxW(nullptr, L"Direct3D 11 could not be started.", L"Kameo RePowered Launcher", MB_ICONERROR);
    return false;
  }
  ::ShowWindow(hwnd, show);
  ::UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ApplyTheme(app.dpi_scale);
  LoadFonts(app);
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  while (!app.quit) {
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
      if (message.message == WM_QUIT) app.quit = true;
    }
    if (app.quit) break;
    if (::IsIconic(hwnd)) {
      ::Sleep(20);
      continue;
    }
    if (g_resize_width != 0 && g_resize_height != 0) {
      ReleaseTarget();
      g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
      g_resize_width = g_resize_height = 0;
      CreateTarget();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawFrame(app);
    ImGui::Render();
    const float clear[4] = {0.02f, 0.03f, 0.09f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_target, nullptr);
    g_context->ClearRenderTargetView(g_target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap_chain->Present(1, 0);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  ReleaseDevice();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(window_class.lpszClassName, instance);
  return true;
}

}  // namespace kameo::launcher
