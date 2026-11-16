// The base of our ImGui windows: drawn with the SDK's overlays from construction until destruction.
//
// The UI system builds each overlay frame on the UI thread, inside the present the game thread is making
// (IUISystem::DrawOverlays), so OnDraw runs on the UI thread while the game thread waits for it. Guest memory can be
// read there through GuestBase(); guest functions cannot be called, because the UI thread has no guest context.
#pragma once

#include "core/x11_names.h"
#include <rex/ui/ui_system.h>
#include <rex/utility.h>

class KameoDialog : public rex::ui::DialogBase {
 public:
  KameoDialog();
  ~KameoDialog() override;

 protected:
  virtual void OnDraw(ImGuiIO& io) = 0;

 private:
  bool Draw(ImGuiIO& io) final;

  rex::ScopeGuard registration_;
};
