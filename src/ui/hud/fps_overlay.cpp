#include "ui/hud/fps_overlay.h"

#include <rex/cvar.h>

#include "core/settings.h"

void KameoFpsOverlay::OnDraw(ImGuiIO& /*io*/) {
  if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
    rex::cvar::SetFlagByName("kameo_fps_counter", rex::cvar::Query<bool>("kameo_fps_counter") ? "false" : "true");
    KameoSaveSettings();
  }
}
