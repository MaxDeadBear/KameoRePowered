// Audio language: overriding XGetLanguage, reloading the string table and speech banks when it changes, and picking the Bink FMV language track.

#include <cstring>

#include <rex/logging.h>

#include "core/hooks.h"
#include "engine/audio.h"
#include "engine/guest_addresses.h"

std::atomic<int32_t> g_kameo_audio_language{-1};
std::atomic<int32_t> g_kameo_language_dirty{0};
std::atomic<int32_t> g_kameo_original_language{-1};
std::atomic<int32_t> g_kameo_startup_language{1};

using namespace kameo::engine;

namespace {

// The loaded string table's handle; LocString_CloseBLOB takes it and locLoadStringTable returns a new one.
constexpr uint32_t kStringTableHandle = 0x827556B8;
// Speech wave banks: a list whose nodes link on at +4 and name themselves at +8.
constexpr uint32_t kWaveBankList = 0x82B732A4;

}  // namespace

// Called from the per-frame game tick once the language option has changed.
void KameoReloadLanguageStringTable(PPCContext& ctx, uint8_t* base) {
  if (g_kameo_language_dirty.exchange(0, std::memory_order_acq_rel) == 0) {
    return;
  }

  int32_t lang = g_kameo_audio_language.load(std::memory_order_acquire);
  if (lang < 1 || lang > 9) {
    lang = g_kameo_original_language.load(std::memory_order_acquire);
  }
  if (lang < 1) {
    return;
  }

  // locLoadStringTable builds its hardcoded filename from this code.
  REX_STORE_U32(kLangCodeGlobal, static_cast<uint32_t>(lang));

  const uint32_t str_handle = REX_LOAD_U32(kStringTableHandle);
  if (str_handle != 0 && str_handle != 0xFFFFFFFF) {
    PPCContext unload_ctx = ctx;
    unload_ctx.r3.u64 = str_handle;
    LocStringCloseBlob(unload_ctx, base);
    REX_STORE_U32(kStringTableHandle, 0xFFFFFFFF);
  }

  PPCContext reload_ctx = ctx;
  LocLoadStringTable(reload_ctx, base);
  REX_STORE_U32(kStringTableHandle, reload_ctx.r3.u32);

  // Resetting a speech bank's load state makes the game reload it from the new language folder; sub_8230C0C8 and sub_8228A5B0 overwrite +44 and +60, so no destructor is called through the COM vtable that holds a truncated host pointer.
  const uint32_t bank_list_head = REX_LOAD_U32(kWaveBankList);
  for (uint32_t node = bank_list_head; node != 0;
       node = REX_LOAD_U32(node + 4)) {
    const char* name = reinterpret_cast<const char*>(base + node + 8);
    const bool is_speech = (strstr(name, "Speech") != nullptr) ||
                           (strstr(name, "_NPC_")  != nullptr) ||
                           (strstr(name, "_Loc_")  != nullptr);
    if (!is_speech) continue;
    if (REX_LOAD_U32(node + 56) == 0) continue;  // already unloaded

    REX_STORE_U32(node + 44, 0);  // XACT bank ptr  -- overwritten by sub_8230C0C8
    REX_STORE_U32(node + 48, 0);  // file buffer    -- overwritten by async load
    REX_STORE_U32(node + 52, 1);  // reload flag=1  -- sub_822ECD10 calls sub_822ECDF8
    REX_STORE_U32(node + 56, 0);  // state=0        -- triggers sub_822ECC30 next tick
    REX_STORE_U32(node + 60, 0);  // voice buf ptr  -- overwritten by sub_8228A5B0
    REX_STORE_U32(node + 64, 0);  // voice buf size -- overwritten on reload
  }
}

// Sets the FMV language track volume, mirroring the switch in sub_82264140 to pick the track from the UI language, then --user_language, then XGetLanguage.
void KameoSetBinkLanguageTrackVolume(PPCRegister& r27, PPCRegister& r29) {
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  const uint32_t bink_handle = REX_LOAD_U32(r29.u32 + 12);
  if (!bink_handle) {
    return;
  }

  // The game stores XGetLanguage (the system default, English) before the opening FMV, so it cannot carry the startup language.
  uint32_t lang_code = REX_LOAD_U32(kLangCodeGlobal);
  const int32_t audio_lang = g_kameo_audio_language.load(std::memory_order_acquire);
  const int32_t startup_lang = g_kameo_startup_language.load(std::memory_order_acquire);
  if (audio_lang >= 1) {
    lang_code = static_cast<uint32_t>(audio_lang);
  } else if (startup_lang > 1) {
    lang_code = static_cast<uint32_t>(startup_lang);
  }
  uint32_t lang_track;
  switch (lang_code) {
    case 2:  lang_track = 18; break;  // Japanese
    case 3:  lang_track = 15; break;  // German
    case 4:  lang_track = 13; break;  // French
    case 5:  lang_track = 16; break;  // Spanish
    case 6:  lang_track = 14; break;  // Italian
    case 10: lang_track = 17; break;  // Korean/Mexican
    default: lang_track = 12; break;  // English
  }

  PPCContext* ctx_ptr = CurrentGuestContext();
  if (!ctx_ptr) {
    return;
  }
  PPCContext ctx = *ctx_ptr;
  ctx.r3.u64 = bink_handle;
  ctx.r4.u64 = lang_track;
  // The volume is passed through unscaled: BinkSetVolume's entry hook applies the FMV and master volumes.
  ctx.r5.u64 = r27.u32;
  BinkSetVolume(ctx, base);
}

// XGetLanguage's result: remembered once for "system default", then replaced by the chosen language.
void KameoOverrideAudioLanguage(PPCRegister& r3) {
  int32_t expected = -1;
  g_kameo_original_language.compare_exchange_strong(
      expected, static_cast<int32_t>(r3.u32),
      std::memory_order_acq_rel, std::memory_order_relaxed);

  const int32_t lang = g_kameo_audio_language.load(std::memory_order_acquire);
  if (lang >= 1 && lang <= 9) {
    // XGetLanguage is called rarely, not per frame.
    REXLOG_INFO("[kameo-lang] override XGetLanguage {} -> {}", r3.u32, lang);
    r3.s64 = lang;
  }
}
