// DLC unlock and the Kameo skin hot-swap: objectAlternativeSetFileInUse is called with the chosen skin's suffix, and its own suffix argument is replaced while a skin is active.

#include <cstring>

#include "core/hooks.h"
#include "engine/dlc/dlc.h"
#include "engine/dlc/dlc_swap.h"
#include "engine/guest_addresses.h"

namespace kameo::engine {
namespace {

// Kameo's model type, the key objectAlternativeSetFileInUse looks skins up by.
constexpr uint32_t kKameoModelType = 0x007183BF;

// Maps an integer swap request to a DLC suffix (1000 -> "xmas1", 42 -> "42"), or nullptr when out of range.
const char* KameoDlcSwapSuffix(int request) {
  if (request == 1000) {
    return "xmas1";
  }
  if (request == 1001) {
    return "std";
  }
  if (request == 1002) {
    return "prototype";
  }
  if (request == 1003) {
    return "alt01";
  }
  if (request == 1004) {
    return "alt02";
  }

  static thread_local char suffix[3]{};
  if (request < 0 || request > 99) {
    return nullptr;
  }

  suffix[0] = static_cast<char>('0' + (request / 10));
  suffix[1] = static_cast<char>('0' + (request % 10));
  suffix[2] = '\0';
  return suffix;
}

}  // namespace

void ProcessPendingDlcSwap(PPCContext& ctx, uint8_t* base) {
  if (g_kameo_pending_standard_swap.exchange(0, std::memory_order_acq_rel) != 0) {
    ClearKameoDlcSuffix();
    g_kameo_stop_next_dlc_model_load.store(1, std::memory_order_release);
    QueueKameoDlcSuffix("67", false);
    return;
  }

  if (g_kameo_dlc_swap_enabled.load(std::memory_order_acquire) == 0) {
    g_kameo_pending_dlc_suffix_ready.store(0, std::memory_order_release);
    g_kameo_pending_dlc_swap.store(-1, std::memory_order_release);
    return;
  }

  char suffix_buffer[64]{};
  const char* suffix = nullptr;
  if (ConsumeKameoPendingDlcSuffix(suffix_buffer, sizeof(suffix_buffer))) {
    suffix = suffix_buffer;
  } else {
    const int request = g_kameo_pending_dlc_swap.exchange(-1, std::memory_order_acq_rel);
    suffix = KameoDlcSwapSuffix(request);
  }
  if (!suffix) {
    return;
  }

  PPCContext call_ctx = ctx;
  call_ctx.r3.u64 = kKameoModelType;
  const uint32_t guest_str = call_ctx.r1.u32 - 64;
  WriteGuestStringAt(base, guest_str, suffix);
  call_ctx.r4.u64 = guest_str;
  ObjectAlternativeSetFileInUse(call_ctx, base);
}

}  // namespace kameo::engine

using namespace kameo::engine;

void KameoUnlockDlc(PPCRegister& r3) {
  r3.s64 = 1;
}

#if KAMEO_TU
// objectAlternativeModelUnlockedCheck(record) in the title update: it takes the skin record rather than its name, and
// notes at +236 the event that unlocks one of the built-in skins (-1 for any other), which the online skin sync reads,
// so it runs first. As KameoUnlockDlc does on the original, a built-in skin counts as unlocked; a downloadable one keeps
// the update's own answer, which is whether its pack is installed.
extern "C" REX_FUNC(sub_8225E158) {
  const uint32_t record = ctx.r3.u32;
  __imp__sub_8225E158(ctx, base);
  if (record && REX_LOAD_U32(record + 236) != 0xFFFFFFFFu) ctx.r3.u64 = 1;
}
#endif

namespace {

void OverrideDlcSelector(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) {
  if (g_kameo_dlc_swap_enabled.load(std::memory_order_acquire) == 0) {
    return;
  }
  if (r3.u32 != kKameoModelType) {
    return;
  }

  char suffix_buffer[64]{};
  const char* suffix = nullptr;
  if (ReadKameoActiveDlcSuffix(suffix_buffer, sizeof(suffix_buffer))) {
    suffix = suffix_buffer;
  } else {
    suffix = KameoDlcSwapSuffix(g_kameo_active_dlc_swap.load(std::memory_order_acquire));
  }
  if (!suffix) {
    return;
  }

  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  const uint32_t guest_string = r1.u32 - 80;
  WriteGuestStringAt(base, guest_string, suffix);
  r4.u64 = guest_string;
}

}  // namespace

// Entry of objectAlternativeSetFileInUse(model type, suffix): Kameo's suffix is replaced with the active skin's, and a
// shader mod costume loads as the character's standard look.
void KameoOverrideDlcSelectorMid(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) {
  OverrideDlcSelector(r3, r4, r1);
  if (uint8_t* base = GuestBase()) SettingSkin(base, r3.u32, r4, r1);
}

bool KameoShouldSkipNextDlcModelLoad() {
  return g_kameo_stop_next_dlc_model_load.exchange(0, std::memory_order_acq_rel) != 0;
}
