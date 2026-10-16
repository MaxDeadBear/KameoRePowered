// The volume and audio-language settings the menus write and the audio hooks in engine/audio.cpp and engine/audio_language.cpp apply.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/ppc.h>

extern std::atomic<int32_t> g_kameo_master_volume;
extern std::atomic<int32_t> g_kameo_sfx_volume;
extern std::atomic<int32_t> g_kameo_music_volume;
extern std::atomic<int32_t> g_kameo_ambience_volume;
extern std::atomic<int32_t> g_kameo_speech_volume;
extern std::atomic<int32_t> g_kameo_fmv_volume;
extern std::atomic<int32_t> g_kameo_audio_language;
extern std::atomic<int32_t> g_kameo_volume_dirty;
extern std::atomic<int32_t> g_kameo_language_dirty;
extern std::atomic<int32_t> g_kameo_original_language;
extern std::atomic<int32_t> g_kameo_startup_language;

// Reloads the string table and speech wave banks after the audio language changes; does nothing unless g_kameo_language_dirty is set.
void KameoReloadLanguageStringTable(PPCContext& ctx, uint8_t* base);
// Replays the cached audioCategorySetVolumeString calls after a volume slider moves; does nothing unless g_kameo_volume_dirty is set.
void KameoReplayCachedVolumes(PPCContext& ctx, uint8_t* base);
