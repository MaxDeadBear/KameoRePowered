#include "ui/hud/fps_overlay.h"

#include "core/cvar.h"

#include "core/settings.h"

void KameoFpsOverlay::OnDraw(ImGuiIO& /*io*/) {
  if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
    kameo::cvar::SetText("kameo_fps_counter", kameo::cvar::GetBool("kameo_fps_counter") ? "false" : "true");
    KameoSaveSettings();
  }
}
