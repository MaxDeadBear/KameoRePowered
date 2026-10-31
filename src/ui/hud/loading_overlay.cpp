#include "ui/hud/loading_overlay.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>

#include <imgui.h>

#include "gpu/pipeline/pso_precache.h"

namespace {

// A start this quick has nothing to explain, so the panel does not flash up.
constexpr float kShowAfterSeconds = 0.4f;
// A game that never draws must not keep the panel up forever.
constexpr float kGiveUpSeconds = 90.0f;
constexpr float kFadeSeconds = 0.35f;

// The launcher's indigo and gold.
constexpr ImVec4 kPanel(0.08f, 0.06f, 0.18f, 1.0f);
constexpr ImVec4 kGold(0.96f, 0.76f, 0.36f, 1.0f);
constexpr ImVec4 kGoldLight(1.0f, 0.93f, 0.68f, 1.0f);
constexpr ImVec4 kGoldDeep(0.77f, 0.50f, 0.16f, 1.0f);
constexpr ImVec4 kText(0.93f, 0.91f, 0.98f, 1.0f);
// Fire, ice, water and earth, lit in turn as the shaders come in.
constexpr ImVec4 kElements[] = {{1.0f, 0.42f, 0.12f, 1.0f}, {0.62f, 0.88f, 1.0f, 1.0f}, {0.20f, 0.52f, 1.0f, 1.0f}, {0.48f, 0.80f, 0.30f, 1.0f}};

ImFont* g_wordmarkFont = nullptr;
ImFont* g_textFont = nullptr;

ImU32 Colour(const ImVec4& colour, float alpha) { return ImGui::GetColorU32(ImVec4(colour.x, colour.y, colour.z, colour.w * alpha)); }

void DrawOrb(ImDrawList* draw, ImVec2 centre, float radius, const ImVec4& colour, float lit, float alpha) {
  // Soft rings for the glow, which no gradient circle in ImGui gives.
  for (int ring = 4; ring > 0; --ring) draw->AddCircleFilled(centre, radius * (1.0f + float(ring) * 0.34f), Colour(colour, 0.11f * lit * alpha), 32);
  draw->AddCircleFilled(centre, radius, Colour(ImVec4(0.09f, 0.07f, 0.20f, 1.0f), alpha), 32);
  draw->AddCircleFilled(centre, radius * 0.86f, Colour(colour, (0.12f + 0.88f * lit) * alpha), 32);
  draw->AddCircle(centre, radius, Colour(kGold, (0.45f + 0.55f * lit) * alpha), 32, 1.0f);
  if (lit > 0.0f) draw->AddCircleFilled(ImVec2(centre.x - radius * 0.28f, centre.y - radius * 0.30f), radius * 0.24f, Colour(ImVec4(1.0f, 1.0f, 1.0f, 0.75f), lit * alpha), 16);
}

void DrawBar(ImDrawList* draw, ImVec2 from, ImVec2 to, float fraction, bool moving, float time, float alpha) {
  const float rounding = (to.y - from.y) * 0.5f;
  const float width = to.x - from.x;
  draw->AddRectFilled(from, to, Colour(ImVec4(0.05f, 0.04f, 0.13f, 1.0f), alpha), rounding);
  ImVec2 fill_from = from;
  ImVec2 fill_to = ImVec2(from.x + width * fraction, to.y);
  if (moving) {
    // No count to show: a band of gold sweeps across instead.
    const float travel = time * 0.55f - std::floor(time * 0.55f);
    fill_from.x = from.x + std::max(0.0f, travel * 1.3f - 0.3f) * width;
    fill_to.x = from.x + std::min(1.0f, travel * 1.3f) * width;
  }
  if (fill_to.x > fill_from.x + 1.0f) {
    draw->AddRectFilled(ImVec2(fill_from.x, from.y - 2.0f), ImVec2(fill_to.x, to.y + 2.0f), Colour(kGold, 0.16f * alpha), rounding + 2.0f);
    draw->PushClipRect(fill_from, fill_to, true);
    draw->AddRectFilled(from, to, Colour(kGold, alpha), rounding);
    draw->AddRectFilledMultiColor(from, ImVec2(to.x, (from.y + to.y) * 0.5f), Colour(kGoldLight, 0.75f * alpha), Colour(kGoldLight, 0.75f * alpha), Colour(kGold, 0.0f), Colour(kGold, 0.0f));
    draw->AddRectFilledMultiColor(ImVec2(from.x, (from.y + to.y) * 0.5f), to, Colour(kGoldDeep, 0.0f), Colour(kGoldDeep, 0.0f), Colour(kGoldDeep, alpha), Colour(kGoldDeep, alpha));
    // A glint running along the gold, so a slow stretch still shows that something is working.
    const float glint = fill_from.x + (fill_to.x - fill_from.x) * (time * 0.8f - std::floor(time * 0.8f));
    const float glint_width = width * 0.12f;
    draw->AddRectFilledMultiColor(ImVec2(glint - glint_width, from.y), ImVec2(glint, to.y), Colour(kGoldLight, 0.0f), Colour(kGoldLight, 0.9f * alpha), Colour(kGoldLight, 0.9f * alpha), Colour(kGoldLight, 0.0f));
    draw->AddRectFilledMultiColor(ImVec2(glint, from.y), ImVec2(glint + glint_width, to.y), Colour(kGoldLight, 0.9f * alpha), Colour(kGoldLight, 0.0f), Colour(kGoldLight, 0.0f), Colour(kGoldLight, 0.9f * alpha));
    draw->PopClipRect();
  }
  draw->AddRect(from, to, Colour(kGold, 0.55f * alpha), rounding, 0, 1.0f);
}

}  // namespace

void KameoLoadingOverlay::ConfigureFonts(ImFontAtlas* atlas) {
#ifdef _WIN32
  // The launcher's faces, where Windows has them.
  const auto load = [atlas](const wchar_t* file, float size) -> ImFont* {
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::path(L"C:\\Windows\\Fonts") / file;
    return std::filesystem::exists(path, ec) ? atlas->AddFontFromFileTTF(path.string().c_str(), size) : nullptr;
  };
  g_wordmarkFont = load(L"palab.ttf", 36.0f);
  g_textFont = load(L"segoeui.ttf", 20.0f);
#else
  (void)atlas;
#endif
}

void KameoLoadingOverlay::OnDraw(ImGuiIO& io) {
  if (finished_) return;
  uint32_t done = 0;
  uint32_t total = 0;
  const bool shaders = kameo::gpu::PipelinePrecacheProgress(&done, &total);
  const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
  const bool over = !shaders && (kameo::gpu::GamePictureShown() || elapsed > kGiveUpSeconds);
  if (elapsed < kShowAfterSeconds) {
    if (over) finished_ = true;
    return;
  }
  if (over && hide_at_ < 0.0f) hide_at_ = elapsed;
  float alpha = std::min((elapsed - kShowAfterSeconds) / kFadeSeconds, 1.0f);
  if (hide_at_ >= 0.0f) {
    alpha = std::min(alpha, 1.0f - (elapsed - hide_at_) / kFadeSeconds);
    if (alpha <= 0.0f) {
      finished_ = true;
      return;
    }
  }

  const float scale = std::max(io.DisplaySize.y / 1080.0f, 0.75f);
  const ImVec2 size(470.0f * scale, 114.0f * scale);
  const float pad = 20.0f * scale;
  const ImVec2 corner(44.0f * scale, io.DisplaySize.y - 44.0f * scale - size.y);
  const ImVec2 end(corner.x + size.x, corner.y + size.y);
  const float rounding = 14.0f * scale;
  // Over everything, and never in the way of the mouse, which a window of its own would have to be told.
  ImDrawList* draw = ImGui::GetForegroundDrawList();

  for (int ring = 3; ring > 0; --ring) {
    const float grow = float(ring) * 3.0f * scale;
    draw->AddRectFilled(ImVec2(corner.x - grow, corner.y - grow), ImVec2(end.x + grow, end.y + grow), Colour(ImVec4(0.45f, 0.28f, 0.95f, 1.0f), 0.08f * alpha), rounding + grow);
  }
  draw->AddRectFilled(corner, end, Colour(kPanel, 0.96f * alpha), rounding);
  draw->AddRect(corner, end, Colour(kGold, 0.85f * alpha), rounding, 0, 1.5f * scale);

  // The wordmark, with a dark offset so it reads over any picture.
  const char* wordmark = "KAMEO";
  const float wordmark_size = 32.0f * scale;
  ImFont* wordmark_font = g_wordmarkFont ? g_wordmarkFont : ImGui::GetFont();
  const ImVec2 wordmark_at(corner.x + pad, corner.y + pad * 0.62f);
  draw->AddText(wordmark_font, wordmark_size, ImVec2(wordmark_at.x + 1.5f * scale, wordmark_at.y + 1.5f * scale), Colour(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.8f * alpha), wordmark);
  draw->AddText(wordmark_font, wordmark_size, wordmark_at, Colour(kGold, alpha), wordmark);

  // One orb lights for each quarter; the one being filled brightens as its quarter goes by, and all of them breathe in turn with no count to show.
  const float fraction = shaders && total > 0 ? float(done) / float(total) : 0.0f;
  const int orbs = int(std::size(kElements));
  const float orb_radius = 10.0f * scale;
  for (int i = 0; i < orbs; ++i) {
    float lit;
    if (shaders) {
      lit = std::clamp(fraction * float(orbs) - float(i), 0.0f, 1.0f);
    } else {
      lit = 0.55f + 0.45f * std::sin(elapsed * 2.6f - float(i) * 0.9f);
    }
    const ImVec2 centre(end.x - pad - orb_radius - float(orbs - 1 - i) * (orb_radius * 3.0f), corner.y + pad * 0.62f + wordmark_size * 0.5f);
    DrawOrb(draw, centre, orb_radius, kElements[i], lit, alpha);
  }

  const float bar_top = corner.y + size.y * 0.53f;
  const float bar_height = 10.0f * scale;
  DrawBar(draw, ImVec2(corner.x + pad, bar_top), ImVec2(end.x - pad, bar_top + bar_height), fraction, !shaders, elapsed, alpha);

  // What is happening in plain words, under the flourish, so nobody takes a long first start for a hang.
  const float text_size = 18.0f * scale;
  ImFont* text_font = g_textFont ? g_textFont : ImGui::GetFont();
  const char* status = shaders ? "Gathering the elements: preparing shaders" : "Awakening the kingdom: loading the game";
  const float text_y = bar_top + bar_height + 8.0f * scale;
  draw->AddText(text_font, text_size, ImVec2(corner.x + pad, text_y), Colour(kText, alpha), status);
  if (shaders && total > 0) {
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%u%%", done * 100 / total);
    const ImVec2 percent_size = text_font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, percent);
    draw->AddText(text_font, text_size, ImVec2(end.x - pad - percent_size.x, text_y), Colour(kGold, alpha), percent);
  }
}
