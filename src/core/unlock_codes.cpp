#include "core/unlock_codes.h"

#include <rex/cvar.h>

#include "core/settings.h"

REXCVAR_DEFINE_INT32(kameo_unlocked_codes, 0, "Kameo", "Features unlocked by cheat codes entered in the pause menu, one bit each: 1 Level Select, 2 the game's cheats, 4 all warriors and upgrades, 8 the collectible tracker.");

bool KameoCodeUnlocked(KameoUnlockCode code) { return (uint32_t(REXCVAR_GET(kameo_unlocked_codes)) & uint32_t(code)) != 0; }

void KameoUnlockFeature(KameoUnlockCode code) {
  REXCVAR_SET(kameo_unlocked_codes, int32_t(uint32_t(REXCVAR_GET(kameo_unlocked_codes)) | uint32_t(code)));
  KameoSaveSettings();
}
