// Audio volumes: each XACT category scaled by its slider and the master, Bink FMV volume, and replaying the game's own volume calls when a slider moves.

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/hooks.h"
#include "engine/audio.h"
#include "engine/guest_addresses.h"

std::atomic<int32_t> g_kameo_master_volume{50};
std::atomic<int32_t> g_kameo_sfx_volume{50};
std::atomic<int32_t> g_kameo_music_volume{50};
std::atomic<int32_t> g_kameo_ambience_volume{50};
std::atomic<int32_t> g_kameo_speech_volume{50};
std::atomic<int32_t> g_kameo_fmv_volume{50};
std::atomic<int32_t> g_kameo_volume_dirty{0};

namespace {

// The most recent audioCategorySetVolumeString(cat_ptr, volume) per category name, replayed when a slider changes.
std::mutex s_volume_snapshot_mutex;
std::unordered_map<std::string, std::pair<uint32_t, double>> s_cat_last_params;

bool KameoIsFmvCategory(const char* cat) {
  return strcmp(cat, "MovieMusic") == 0 ||
         strcmp(cat, "MovieSfx") == 0;
}

bool KameoIsMusicCategory(const char* cat) {
  return strcmp(cat, "Music") == 0 ||
         strcmp(cat, "LevelMusic") == 0 ||
         strcmp(cat, "NonLevelMusic") == 0;
}

bool KameoIsSfxCategory(const char* cat) {
  return strcmp(cat, "Sfx") == 0 ||
         strcmp(cat, "GameSfx") == 0 ||
         strcmp(cat, "MusicStingSfx") == 0 ||
         strcmp(cat, "InterfaceSfx") == 0;
}

bool KameoIsAmbienceCategory(const char* cat) {
  return strcmp(cat, "Ambience") == 0 ||
         strcmp(cat, "LevelAmbience") == 0 ||
         strcmp(cat, "StealthAmbience") == 0;
}

bool KameoIsSpeechCategory(const char* cat) {
  return strcmp(cat, "Speech") == 0 ||
         strcmp(cat, "InGameSpeech") == 0 ||
         strcmp(cat, "InterfaceSpeech") == 0;
}

}  // namespace

// Called from the per-frame game tick.
void KameoReplayCachedVolumes(PPCContext& ctx, uint8_t* base) {
  if (g_kameo_volume_dirty.exchange(0, std::memory_order_acq_rel) == 0) {
    return;
  }

  std::vector<std::pair<uint32_t, double>> entries;
  {
    std::lock_guard<std::mutex> lock(s_volume_snapshot_mutex);
    entries.reserve(s_cat_last_params.size());
    for (auto& [name, p] : s_cat_last_params) {
      entries.push_back(p);
    }
  }
  for (auto& [cat_ptr, orig_f1] : entries) {
    PPCContext call_ctx = ctx;
    call_ctx.r3.u64 = cat_ptr;
    call_ctx.f1.f64 = orig_f1;
    kameo::engine::AudioCategorySetVolumeString(call_ctx, base);
  }
}

// Entry of audioCategorySetVolumeString(category name, volume).
void KameoOverrideVolume(PPCRegister& r3, PPCRegister& f1) {
  if (r3.u32 == 0) {
    return;
  }

  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  const char* cat = reinterpret_cast<const char*>(base + r3.u32);

  {
    std::lock_guard<std::mutex> lock(s_volume_snapshot_mutex);
    s_cat_last_params[std::string(cat)] = {r3.u32, f1.f64};
  }

  int32_t scale_pct = 100;
  if (KameoIsFmvCategory(cat)) {
    scale_pct = g_kameo_fmv_volume.load(std::memory_order_acquire);
  } else if (KameoIsMusicCategory(cat)) {
    scale_pct = g_kameo_music_volume.load(std::memory_order_acquire);
  } else if (KameoIsSfxCategory(cat)) {
    scale_pct = g_kameo_sfx_volume.load(std::memory_order_acquire);
  } else if (KameoIsAmbienceCategory(cat)) {
    scale_pct = g_kameo_ambience_volume.load(std::memory_order_acquire);
  } else if (KameoIsSpeechCategory(cat)) {
    scale_pct = g_kameo_speech_volume.load(std::memory_order_acquire);
  }

  scale_pct = scale_pct * g_kameo_master_volume.load(std::memory_order_acquire) / 100;
  if (scale_pct == 100) {
    return;
  }

  const float scaled = static_cast<float>(f1.f64) * (static_cast<float>(scale_pct) / 100.0f);
  f1.f64 = static_cast<double>(scaled);
}

// Entry of BinkSetVolume(bink, track, volume).
void KameoOverrideBinkVolume(PPCRegister& r5) {
  // 0 is the game silencing the track at the end of playback.
  if (r5.u32 == 0) {
    return;
  }
  const int32_t fmv_pct = g_kameo_fmv_volume.load(std::memory_order_acquire) * g_kameo_master_volume.load(std::memory_order_acquire) / 100;
  if (fmv_pct == 100) {
    return;
  }
  r5.u32 = r5.u32 * static_cast<uint32_t>(fmv_pct) / 100;
}
