// The midasm hooks config/hooks/*.toml names; the generated code calls them by these exact global names and signatures.
#pragma once

#include <rex/ppc.h>

void KameoUnlockDlc(PPCRegister& r3);
void KameoGameTick();
void KameoOverrideDlcSelectorMid(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1);
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
void KameoSplitScreenTick();
void KameoSaveGameSplitGate(PPCRegister& r11);
void KameoStorybookSplitGate(PPCRegister& r11);
void KameoOverridePlayer2Type(PPCRegister& r4);
void KameoCaptureWarriorSack(PPCRegister& r3);
void KameoUnlockAllWarriors(PPCRegister& r3);
void KameoViewportGate(PPCRegister& r10, PPCRegister& r11);
void KameoGuardEffectsDispatch(PPCRegister& r8);
void KameoAddScore(PPCRegister& r11);
void KameoOneShotDamage(PPCRegister& r27, PPCRegister& f31);
