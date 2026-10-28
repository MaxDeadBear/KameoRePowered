// Controls page: input mode and the mouse camera.

#include <cmath>

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

void PageControls(App& app) {
  PageTitle(app, "Controls", nullptr);
  RestoreButton(app, {"kameo_input_mode", "mnk_mouse", "mnk_sensitivity"});
  if (BeginSection("controls", "Input")) {
    ChoiceRow(app, "Input Mode", "kameo_input_mode", {{"Keyboard & Mouse", 0}, {"Gamepad", 1}});
    BoolRow(app, "Mouse Camera", "mnk_mouse", "The mouse turns the camera in gameplay. Menus always get a pointer.");
    Row("Mouse Sensitivity");
    float sensitivity = float(app.settings.GetDouble("mnk_sensitivity"));
    ImGui::SliderFloat("##mnk_sensitivity", &sensitivity, 0.1f, 10.0f, "%.1f");
    if (std::abs(sensitivity - float(app.settings.GetDouble("mnk_sensitivity"))) >= 0.05f) app.settings.SetDouble("mnk_sensitivity", std::round(sensitivity * 10.0f) / 10.0f);
  }
  EndSection();
  ImGui::TextColored(kTextDim, "Keys are changed on the Key Bindings page.");
}

}  // namespace kameo::launcher
