#include "ui/hud/achievement_icons.h"

#include <algorithm>
#include <vector>

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "core/hooks.h"
#include "gpu/config.h"
#include "gpu/draw/draw_constants.h"
#include "gpu/graphics_system.h"
#include "gpu/replay/screen_layout.h"
#include "ui/menu_pointer/menu_ui_state.h"
#include "ui/pause_menu/pause_menu.h"
#include "ui/text_window.h"

using namespace kameo::ui;

namespace {

// The icon sits inside the box at the start of the row, in the room the page's indent leaves, a little smaller than the row's two lines (label and description) and centred on them.
constexpr float kIconInset = 2.0f;
constexpr float kIconLines = 1.55f;
constexpr float kIconDrop = 0.18f;  // of a line height, down from the label's top, so the art is centred on its two lines with air above and below

}  // namespace

void KameoAchievementIcons::OnDraw(ImGuiIO& io) {
  uint32_t text_object = 0;
  std::vector<KameoAchievementIconRow> rows;
  if (!KameoAchievementsPageShowing(&text_object, &rows)) return;
  if (!cache_) {
    if (!runtime_ || !immediate_drawer_) return;
    cache_ = std::make_unique<rex::ui::AchievementIconCache>(immediate_drawer_, runtime_);
  }
  // Where the window last drew its selectable lines; stale once the menu has gone, so nothing is drawn over a closed menu.
  const std::vector<DrawnLine> lines = DrawnLinesOf(text_object);
  if (lines.empty()) return;
  uint8_t* base = GuestBase();
  rex::system::KernelState* kernel = rex::system::kernel_state();
  if (!base || !kernel) return;
  // Where the text starts: the window's left edge, which is inside its margin and frame; the other edges clip the icons as the game clips its lines.
  const float text_left = float(int16_t(REX_LOAD_U16(text_object + kTextLeft)));
  const float text_right = float(int16_t(REX_LOAD_U16(text_object + kTextRight)));
  const float text_top = float(int16_t(REX_LOAD_U16(text_object + kTextTop)));
  const float text_bottom = float(int16_t(REX_LOAD_U16(text_object + kTextBottom)));

  // The inverse of the pointer's window-to-layout mapping: the letterboxed guest frame, then the widescreen un-stretch the HUD layout applies about the centre.
  const uint32_t width = uint32_t(io.DisplaySize.x);
  const uint32_t height = uint32_t(io.DisplaySize.y);
  if (width == 0 || height == 0) return;
  const kameo::gpu::detail::GuestToHost t = kameo::gpu::detail::GuestToHostTransform(width, height);
  float unstretch = 1.0f;
  if (REXCVAR_GET(kameo_gfx_widescreen) && REXCVAR_GET(kameo_hud_layout) != 0) {
    if (auto* gfx = kameo::gpu::GraphicsSystem::instance()) unstretch = float(std::max(gfx->DisplayAspect() / (16.0 / 9.0), 1.0));
  }
  const auto to_window = [&](float layout_x, float layout_y) {
    const float x = 320.0f + (layout_x - 320.0f) / unstretch;
    return ImVec2(t.origin_x + x * (float(kameo::gpu::detail::kGuestWidth) / 640.0f) * t.scale_x, t.origin_y + layout_y * (float(kameo::gpu::detail::kGuestHeight) / 480.0f) * t.scale_y);
  };

  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  draw->PushClipRect(to_window(text_left, text_top), to_window(text_right, text_bottom), true);
  for (const DrawnLine& line : lines) {
    if (line.index < 0 || size_t(line.index) >= rows.size()) continue;
    const KameoAchievementIconRow& row = rows[size_t(line.index)];
    const auto info = kernel->achievements().FindAchievement(row.achievement_id);
    if (!info) continue;
    rex::ui::ImmediateTexture* icon = cache_->GetIcon(*info);
    if (!icon) continue;
    const float line_height = line.bottom - line.top;
    const float size = line_height * kIconLines;
    const float left = text_left + kIconInset;
    const float top = line.top + line_height * kIconDrop;
    const ImVec2 p0 = to_window(left, top);
    const ImVec2 p1 = to_window(left + size, top + size);
    // A locked achievement's art is dimmed, which is how the console showed it; the art is drawn square, as the console did.
    const ImU32 tint = row.unlocked ? IM_COL32(255, 255, 255, 255) : IM_COL32(96, 96, 96, 230);
    draw->AddImage(reinterpret_cast<ImTextureID>(icon), p0, p1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
  }
  draw->PopClipRect();
}
