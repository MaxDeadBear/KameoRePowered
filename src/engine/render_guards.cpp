// Crash guards inside the game's own render code.

#include <rex/logging/macros.h>

#include "core/hooks.h"
#include "engine/splitscreen_internal.h"

namespace {

#if KAMEO_TU
// The title update's .text spans 0x820B0000-0x82707F1C and its Bink code runs to 0x827181F8.
constexpr uint32_t kCodeBegin = 0x820B0000u;
constexpr uint32_t kCodeEnd = 0x82720000u;
#else
// .text spans 0x820B0000-0x826CF6BC and Bink code runs to 0x826DF9F8.
constexpr uint32_t kCodeBegin = 0x820B0000u;
constexpr uint32_t kCodeEnd = 0x826E0000u;
#endif
// A function consisting solely of `blr`.
constexpr uint32_t kBareReturn = 0x823BB178u;

}  // namespace

// depthEffects_render's `lwzx r8, table[idx]; mtctr r8; bctrl`: after a viewport-count change the index can read flag words past the table, so a non-code target becomes a no-op.
void KameoGuardEffectsDispatch(PPCRegister& r8) {
  const uint32_t target = r8.u32;
  if (target >= kCodeBegin && target < kCodeEnd) {
    return;
  }
  r8.u64 = kBareReturn;
}
