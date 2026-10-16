// The per-frame pump on the game thread, where work queued by the UI and host threads is applied to the guest.

#include "core/hooks.h"
#include "engine/audio.h"
#include "engine/dlc/dlc.h"
#include "engine/guest_addresses.h"
#include "engine/super_jump.h"
#include "input/input.h"
#include "ui/hud/fps_counter.h"

// Hooked at 0x8224F700, once per game tick.
void KameoGameTick() {
  PPCContext* ctx_ptr = CurrentGuestContext();
  if (!ctx_ptr) {
    return;
  }

  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  KameoReloadLanguageStringTable(*ctx_ptr, base);
  KameoReplayCachedVolumes(*ctx_ptr, base);
  kameo::engine::ProcessPendingDlcSwap(*ctx_ptr, base);
  KameoUpdateSuperJumpFloat(*ctx_ptr, base);
  KameoTickInput();
  KameoTickFpsCounter(*ctx_ptr, base);
}
