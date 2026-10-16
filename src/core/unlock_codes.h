// Features a cheat code entered in the pause menu reveals; once entered they stay unlocked in kameo_settings.toml.
#pragma once

#include <cstdint>

enum class KameoUnlockCode : uint32_t {
  LevelSelect = 1,
  GameCheats = 2,
  Warriors = 4,
  CollectibleTracker = 8,
};

bool KameoCodeUnlocked(KameoUnlockCode code);
void KameoUnlockFeature(KameoUnlockCode code);
