#include "ui/dialog.h"

#include <rex/module.h>

KameoDialog::KameoDialog() {
  if (rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem)) registration_ = ui->AddDialog(*this);
}

KameoDialog::~KameoDialog() = default;

bool KameoDialog::Draw(ImGuiIO& io) {
  OnDraw(io);
  return true;
}
