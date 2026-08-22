#pragma once

#include <atomic>

#include <rex/ppc.h>

extern std::atomic<uint32_t> g_kameo_infinite_energy_enabled;
extern std::atomic<uint32_t> g_kameo_infinite_health_enabled;

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

void KameoUnlockDlc(PPCRegister& r3);
void KameoProcessPendingDlcSwapMid();
void KameoOverrideDlcSelectorMid(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1);
void KameoForceReloadOnSameRecord(PPCCRRegister& cr6);
bool KameoShouldSkipNextDlcModelLoad();
void KameoInfiniteEnergy(PPCRegister& r3);
void KameoInfiniteEnergyCurrent(PPCRegister& r3);
void KameoInfiniteEnergyMax(PPCRegister& r3);
void KameoRefillMeterFloat(PPCRegister& dst, PPCRegister& src);
void KameoRefillMeterFloatPlus4(PPCRegister& dst, PPCRegister& src);
void KameoRefillHealth(PPCRegister& r29);
void KameoOverrideVolume(PPCRegister& r3, PPCRegister& f1);
void KameoOverrideBinkVolume(PPCRegister& r5);
void KameoSetBinkLanguageTrackVolume(PPCRegister& r27, PPCRegister& r29);
void KameoOverrideAudioLanguage(PPCRegister& r3);
void KameoForceStorySplitScreen();
void KameoSaveGameSplitGate(PPCRegister& r11);
void KameoStorybookSplitGate(PPCRegister& r11);
void KameoOverridePlayer2Type(PPCRegister& r4);
void KameoProbeKaiAsk(PPCRegister& r3, PPCRegister& r5, PPCRegister& r6);
void KameoProbeMorphValid(PPCRegister& r3, PPCRegister& r4);
void KameoUnlockAllWarriors(PPCRegister& r3);
void KameoViewportGate(PPCRegister& r10, PPCRegister& r11);
void KameoGuardEffectsDispatch(PPCRegister& r8);
