#include "input/sdk_settings.h"

#include <rex/hid/input_system.h>
#include <rex/module.h>

namespace {

// Stands in while there is no input system (before the app starts its systems and after it stops them), so a read then gets the defaults.
struct Detached final : rex::SettingOwner {
  Detached() : SettingOwner("kameo.detached") {}
  REX_SETTING(bool, mnk_mouse, false, "");
  REX_SETTING(rex::f64, mnk_sensitivity, 1.0, "");
};

Detached& NoInput() {
  static Detached detached;
  return detached;
}

}  // namespace

rex::Setting<bool>& MnkMouse() {
  rex::hid::IInputSystem* input = REX_INTERFACE(rex::hid::IInputSystem);
  return input ? input->GetSettings().mnk_mouse : NoInput().mnk_mouse;
}

rex::Setting<rex::f64>& MnkSensitivity() {
  rex::hid::IInputSystem* input = REX_INTERFACE(rex::hid::IInputSystem);
  return input ? input->GetSettings().mnk_sensitivity : NoInput().mnk_sensitivity;
}
