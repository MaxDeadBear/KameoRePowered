#include "core/cheat_guard.h"

#include "mods/mods.h"

#include <mutex>

#include "core/cvar.h"
#include <rex/logging/log_category.h>

#include "core/save_backup.h"
#include "core/settings.h"
#include "core/unlock_codes.h"

KCVAR_DECLARE(bool, kameo_unlock_all_extras);
KCVAR_DECLARE(bool, kameo_unlock_all_warriors);
KCVAR_DEFINE_BOOL(kameo_cheats_used, false, "Kameo", "Set when a cheat first takes effect, which keeps leaderboard scores off until the launcher restores the saves from before it.");

namespace {

std::mutex g_mutex;
std::filesystem::path g_contentRoot;

// A cheat counts once its code is entered and its option is on, which is when it changes the game. Level Select and the collectible tracker change no scores.
bool CheatInEffect() {
  return (KCVAR_GET(kameo_unlock_all_extras) && KameoCodeUnlocked(KameoUnlockCode::GameCheats)) || (KCVAR_GET(kameo_unlock_all_warriors) && KameoCodeUnlocked(KameoUnlockCode::Warriors));
}

}  // namespace

void KameoInitCheatGuard(const std::filesystem::path& content_root) {
  {
    std::lock_guard lock(g_mutex);
    g_contentRoot = content_root;
  }
  // A cheat the launcher turned on. Writing the settings now records the mark, because a later start that missed it would back up the cheated saves over the clean ones.
  if (!KCVAR_GET(kameo_cheats_used) && CheatInEffect()) KameoSaveSettings();
}

void KameoNoteCheatSettings() {
  std::lock_guard lock(g_mutex);
  if (g_contentRoot.empty() || KCVAR_GET(kameo_cheats_used) || !CheatInEffect()) return;
  std::string error;
  if (kameo::save_backup::BackUp(g_contentRoot, error)) {
    REXLOG_INFO("[kameo] a cheat is on: saves backed up to {}", (g_contentRoot / "save_backups").string());
  } else {
    REXLOG_WARN("[kameo] a cheat is on, but the saves could not be backed up: {}", error);
  }
  KCVAR_SET(kameo_cheats_used, true);
}

std::string KameoLeaderboardBlock() {
  // For this session only: nothing is marked in the saves, and switching the mods off and restarting posts scores again.
  if (KameoModsEnabled()) return "Scores are off while mods from the mods folder are switched on. Switch them off in the mods menu (F5) and restart to post scores.";
  if (!KCVAR_GET(kameo_cheats_used) && !CheatInEffect()) return {};
  return "Scores are off because cheats were used. The launcher's Mods page can restore your saves from before them.";
}
