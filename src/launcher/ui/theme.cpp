// The launcher's palette applied to ImGui, its fonts, and the animated night-sky background.

#include <cmath>
#include <filesystem>

#include "launcher/ui/widgets.h"

namespace kameo::launcher {

void ApplyTheme(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  style.WindowPadding = ImVec2(18, 16);
  style.FramePadding = ImVec2(12, 7);
  style.ItemSpacing = ImVec2(10, 9);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.ScrollbarSize = 12;
  style.GrabMinSize = 14;
  style.WindowRounding = 0;
  style.ChildRounding = 12;
  style.FrameRounding = 8;
  style.PopupRounding = 8;
  style.ScrollbarRounding = 8;
  style.GrabRounding = 8;
  style.WindowBorderSize = 0;
  style.ChildBorderSize = 1;
  style.FrameBorderSize = 0;
  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = kText;
  colors[ImGuiCol_TextDisabled] = kTextDim;
  colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.07f, 0.20f, 0.72f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.08f, 0.22f, 0.98f);
  colors[ImGuiCol_Border] = ImVec4(0.55f, 0.45f, 0.90f, 0.28f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.14f, 0.36f, 0.85f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.20f, 0.50f, 0.90f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.25f, 0.60f, 1.0f);
  colors[ImGuiCol_Button] = ImVec4(0.27f, 0.20f, 0.55f, 0.90f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.29f, 0.74f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.46f, 0.36f, 0.86f, 1.0f);
  colors[ImGuiCol_Header] = ImVec4(0.30f, 0.23f, 0.62f, 0.75f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.38f, 0.29f, 0.74f, 0.85f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.46f, 0.36f, 0.86f, 1.0f);
  colors[ImGuiCol_SliderGrab] = kGold;
  colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.85f, 0.50f, 1.0f);
  colors[ImGuiCol_CheckMark] = kGold;
  colors[ImGuiCol_Separator] = ImVec4(0.55f, 0.45f, 0.90f, 0.25f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.40f, 0.32f, 0.70f, 0.60f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.50f, 0.40f, 0.85f, 0.80f);
  colors[ImGuiCol_ScrollbarGrabActive] = kGold;
  colors[ImGuiCol_PlotHistogram] = kGold;
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.55f, 0.45f, 0.90f, 0.15f);
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
}

void LoadFonts(App& app) {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  const std::filesystem::path fonts = std::filesystem::path(L"C:\\Windows\\Fonts");
  const auto load = [&](const wchar_t* file, float size) -> ImFont* {
    std::error_code ec;
    if (std::filesystem::exists(fonts / file, ec)) return io.Fonts->AddFontFromFileTTF((fonts / file).string().c_str(), size);
    return nullptr;
  };
  app.body = load(L"segoeui.ttf", 18.0f);
  if (!app.body) app.body = io.Fonts->AddFontDefault();
  app.heading = load(L"palab.ttf", 30.0f);
  if (!app.heading) app.heading = app.body;
  ImGui::GetStyle().FontSizeBase = 18.0f;
}

// The window's ground: a night-sky gradient, two soft glows and a scatter of twinkling wisps.
void DrawBackground(ImDrawList* draw, ImVec2 size, float time) {
  draw->AddRectFilledMultiColor(ImVec2(0, 0), size, kGroundTop, kGroundTop, kGroundBottom, kGroundBottom);
  const auto glow = [&](ImVec2 centre, float radius, ImU32 colour) {
    for (int ring = 10; ring > 0; --ring) {
      const float t = float(ring) / 10.0f;
      const int alpha = int(((colour >> IM_COL32_A_SHIFT) & 0xFF) * (1.0f - t) * 0.35f);
      draw->AddCircleFilled(centre, radius * t, (colour & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT), 48);
    }
  };
  glow(ImVec2(size.x * 0.78f, size.y * 0.18f), size.y * 0.75f, IM_COL32(120, 70, 230, 255));
  glow(ImVec2(size.x * 0.20f, size.y * 1.05f), size.y * 0.70f, IM_COL32(40, 150, 255, 200));
  for (int i = 0; i < 70; ++i) {
    const float seed = float(i) * 12.9898f;
    const float x = std::fmod(std::abs(std::sin(seed) * 43758.5453f), 1.0f) * size.x;
    const float y = std::fmod(std::abs(std::sin(seed * 1.7f) * 24634.6345f), 1.0f) * size.y;
    const float twinkle = 0.5f + 0.5f * std::sin(time * (0.6f + std::fmod(seed, 1.3f)) + seed);
    const float radius = 0.8f + 1.6f * std::fmod(std::abs(std::sin(seed * 3.1f)), 1.0f);
    const ImU32 colour = i % 3 == 0 ? IM_COL32(255, 220, 140, int(200 * twinkle)) : IM_COL32(170, 210, 255, int(170 * twinkle));
    draw->AddCircleFilled(ImVec2(x, y), radius, colour, 8);
    if (radius > 1.8f) {
      draw->AddLine(ImVec2(x - radius * 3, y), ImVec2(x + radius * 3, y), colour, 1.0f);
      draw->AddLine(ImVec2(x, y - radius * 3), ImVec2(x, y + radius * 3), colour, 1.0f);
    }
  }
}

}  // namespace kameo::launcher
