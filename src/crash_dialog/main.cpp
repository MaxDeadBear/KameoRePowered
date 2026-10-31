// Kameo RePowered crash dialog: asks the player whether to send a crash to the developers. The game's crash reporter (src/core/crash_reporter.cpp) starts it from the crashed process and waits for its answer. It is drawn with the launcher's window and theme, but is its own executable because the launcher is left alone when a player switches builds and this has to match the game beside it.

#include <cwchar>

#include "crash_dialog/crash_dialog.h"
#include "imgui_backends/imgui_impl_win32.h"
#include "launcher/ui/widgets.h"
#include "launcher/window.h"

namespace {

using namespace kameo::launcher;

constexpr double kButtonDelaySeconds = 2.0;
constexpr const char* kQuestion = "Would you like to send a crash report?";
constexpr const char* kWhy =
    "This is completely optional, and nothing is sent unless you choose Send Report. "
    "A report shows the developers what went wrong, so crashes can get fixed even when nobody files a bug report.";
constexpr const char* kContents =
    "It holds the game's log from this session, the crash's call stacks and the game and OS versions. "
    "It can show file paths, including your Windows user name if the game is in your user folder. "
    "The call stacks hold small pieces of the game's memory, which could rarely include your RexLiveless sign-in.";

// The crash reporter has already cloaked the game's window, which leaves it on the taskbar. This hides it properly when its thread is still alive to do so, and ShowWindowAsync because a plain ShowWindow would wait on a thread that may have died.
BOOL CALLBACK HideGameWindow(HWND window, LPARAM game) {
  DWORD owner = 0;
  GetWindowThreadProcessId(window, &owner);
  if (owner == static_cast<DWORD>(game) && IsWindowVisible(window)) ShowWindowAsync(window, SW_HIDE);
  return TRUE;
}

void DrawDialog(WindowState& window, bool& send) {
  const ImGuiIO& io = ImGui::GetIO();
  DrawBackground(ImGui::GetBackgroundDrawList(), io.DisplaySize, float(ImGui::GetTime()));
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("crash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  ImGui::PushFont(window.heading, 30.0f);
  ImGui::TextColored(kGold, "Kameo has crashed");
  ImGui::PopFont();
  ImGui::Spacing();

  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 button(ImGui::GetFontSize() * 8.0f, ImGui::GetFrameHeight() * 1.2f);
  // The launcher's card made nearly solid, with no dim text, because a player has to be able to read what they are agreeing to.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.05f, 0.15f, 0.97f));
  ImGui::BeginChild("message", ImVec2(0, -(button.y + style.ItemSpacing.y)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleColor();
  ImGui::PushFont(window.body, 20.0f);
  ImGui::TextWrapped("%s", kQuestion);
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::TextWrapped("%s", kWhy);
  ImGui::Spacing();
  ImGui::TextWrapped("%s", kContents);
  ImGui::EndChild();

  ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - style.WindowPadding.x - button.x * 2.0f - style.ItemSpacing.x, ImGui::GetWindowHeight() - style.WindowPadding.y - button.y));
  // Escape can only decline: a player mashing keys as the game dies should not send anything.
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) window.quit = true;
  // The window opens under a player who may still be clicking at the game, and a report must never go out by accident.
  ImGui::BeginDisabled(ImGui::GetTime() < kButtonDelaySeconds);
  if (ImGui::Button("Don't Send", button)) window.quit = true;
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, kGold);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.85f, 0.50f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.77f, 0.50f, 0.16f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.16f, 0.08f, 0.04f, 1.0f));
  if (ImGui::Button("Send Report", button)) {
    send = true;
    window.quit = true;
  }
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();
  ImGui::End();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
  ImGui_ImplWin32_EnableDpiAwareness();
  // The game's process id, which is all it passes.
  const DWORD game = wcstoul(command_line, nullptr, 10);
  if (game != 0) EnumWindows(HideGameWindow, static_cast<LPARAM>(game));
  ClipCursor(nullptr);

  WindowState window;
  bool send = false;
  const WindowOptions options = {.title = L"Kameo RePowered", .width = 620, .height = 420, .dialog = true};
  if (!RunWindow(instance, show, options, window, [&] { DrawDialog(window, send); })) {
    // The crash may have taken the GPU with it, which is no reason not to ask.
    send = MessageBoxW(nullptr, L"Kameo has crashed.\n\nWould you like to send a crash report? This is completely optional, and nothing is sent unless you choose Yes. A report shows the developers what went wrong, so crashes can get fixed.", L"Kameo RePowered", MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
  }
  return send ? static_cast<int>(kCrashDialogSendReport) : 0;
}
