// Controls page: input mode and the mouse camera.

#include <cmath>

#include "core/localization.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

using l10n::Tr;

void PageControls(App& app) {
  PageTitle(app, Tr("Controls"), nullptr);
  RestoreButton(app, {"kameo_input_mode", "mnk_mouse", "mnk_sensitivity"});
  if (BeginSection("controls", Tr("Input"))) {
    ChoiceRow(app, Tr("Input Mode"), "kameo_input_mode", {{Tr("Keyboard & Mouse"), 0}, {Tr("Gamepad"), 1}});
    BoolRow(app, Tr("Mouse Camera"), "mnk_mouse", Tr("The mouse turns the camera in gameplay. Menus always get a pointer."));
    Row(Tr("Mouse Sensitivity"));
    float sensitivity = float(app.settings.GetDouble("mnk_sensitivity"));
    ImGui::SliderFloat("##mnk_sensitivity", &sensitivity, 0.1f, 10.0f, "%.1f");
    if (std::abs(sensitivity - float(app.settings.GetDouble("mnk_sensitivity"))) >= 0.05f) app.settings.SetDouble("mnk_sensitivity", std::round(sensitivity * 10.0f) / 10.0f);
  }
  EndSection();
  ImGui::TextColored(kTextDim, "%s", Tr("Keys are changed on the Key Bindings page."));
}

}  // namespace kameo::launcher
