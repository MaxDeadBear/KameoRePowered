// Online lobby diagnostics: why the host's Play stays unavailable.
//
// KamNetworkIsHostReadyToStartGame() decides whether the host may start a networked game; the lobby asks it every frame.
// It answers yes only when both player slots are filled, the session object exists and is flagged at +104, the lobby
// state is 4, the player count matches the mode, and for two players the remote player's state is also 4, its second
// word is set and a blocking flag is clear (the title update adds one more word that must be zero). Each time any of
// those inputs changes, they are logged, so a lobby that never lets the host start shows which one holds it back.

#include <array>
#include <cstdint>
#include <string>

#include <rex/logging.h>

#include "core/guest.h"
#include "engine/dlc/dlc.h"
#include "kameorepowered_init.h"

namespace {

struct LobbyAddresses {
  uint32_t player_slots;    // two player pointers; a slot counts when its player's byte +8 is 0
  uint32_t lobby;           // lobby state word; the session object pointer is at +32
  uint32_t tu_blocker;      // title update only: must be 0
  uint32_t required;        // how many players the mode needs
  uint32_t remote_state;    // the remote player's state, 4 when ready
  uint32_t remote_word;     // must be non-zero with two players
  uint32_t start_blocker;   // must be 0 with two players
};

#if KAMEO_TU
// KamNetworkIsHostReadyToStartGame in the title update (822B0FB8).
constexpr LobbyAddresses kLobby = {0x828EB600, 0x82BCF18C, 0x82BCF154, 0x8278C1AC, 0x82BCF130, 0x82BCF134, 0x828EB420};
#else
// KamNetworkIsHostReadyToStartGame (8229F8B8).
constexpr LobbyAddresses kLobby = {0x8288EAEC, 0x82B71BB4, 0, 0x8273A880, 0x82B71B84, 0x82B71B88, 0x8288E8A4};
#endif

uint32_t PlayerCounted(uint8_t* base, uint32_t slot) {
  const uint32_t player = REX_LOAD_U32(slot);
  return player && REX_LOAD_U8(player + 8) == 0 ? 1 : 0;
}

#if KAMEO_TU
std::string GuestText(uint8_t* base, uint32_t address) {
  std::string text;
  for (uint32_t i = 0; address && i < 40; ++i) {
    const char c = char(REX_LOAD_U8(address + i));
    if (!c) break;
    text.push_back(c);
  }
  return text;
}
#endif

}  // namespace

#if KAMEO_TU
// Skin sync in an online game, new in the title update. A skin change sends the other console the model and its file
// suffix (822B32C0), the other console's handler (822D1238) answers or applies it through
// objectAlternativeSetFileInUse (8225F088), and only a matching answer marks the skin in use confirmed (+240 = 1),
// which the next skin step waits for. Each step is logged, so a skin line that stops changing shows which one is lost.

// Sends a skin-sync message: (kind, model type, suffix, have, flag).
extern "C" REX_FUNC(sub_822B32C0) {
  REXLOG_INFO("[kameo] skin sync: send kind {} model {:08X} suffix '{}' have {} flag {}", ctx.r3.u32, ctx.r4.u32,
              GuestText(base, ctx.r5.u32), ctx.r6.u32, ctx.r7.u32);
  __imp__sub_822B32C0(ctx, base);
}

// The skin-sync message's handler: +40 retries, +44 kind, +48 model type, +52 suffix, +60 and +64 what it applies.
extern "C" REX_FUNC(sub_822D1238) {
  const uint32_t message = ctx.r3.u32;
  if (kameo::engine::HandleJoinerSkinMessage(base, message)) {
    ctx.r3.u64 = 1;
    return;
  }
  const uint32_t retries = message ? REX_LOAD_U32(message + 40) : 0;
  const uint32_t kind = message ? REX_LOAD_U32(message + 44) : 0;
  const uint32_t model = message ? REX_LOAD_U32(message + 48) : 0;
  const std::string suffix = message ? GuestText(base, message + 52) : std::string();
  const uint32_t have = message ? REX_LOAD_U32(message + 60) : 0;
  const uint32_t flag = message ? REX_LOAD_U32(message + 64) : 0;
  const uint32_t lobby_state = REX_LOAD_U32(0x82BCF18C);
  const uint32_t dlc_enumerating = REX_LOAD_U32(0x827B377C);
  __imp__sub_822D1238(ctx, base);
  REXLOG_INFO("[kameo] skin sync: received kind {} model {:08X} suffix '{}' have {} flag {} (lobby state {}, DLC listing {}, try {}) -> {}",
              kind, model, suffix, have, flag, lobby_state, dlc_enumerating, retries, ctx.r3.u32 ? "done" : "retry later");
}

// objectAlternativeSetFileInUse(model type, suffix, have, flag): flag 1 confirms the skin in use when the suffix
// names it.
extern "C" REX_FUNC(sub_8225F088) {
  const uint32_t model = ctx.r3.u32;
  const std::string suffix = GuestText(base, ctx.r4.u32);
  const uint32_t have = ctx.r5.u32;
  const uint32_t flag = ctx.r6.u32;
  __imp__sub_8225F088(ctx, base);
  REXLOG_INFO("[kameo] skin sync: set file in use model {:08X} suffix '{}' have {} flag {} -> {}", model, suffix, have, flag,
              ctx.r3.u32);
}
#endif

#if KAMEO_TU
extern "C" REX_FUNC(sub_822B0FB8) {
  __imp__sub_822B0FB8(ctx, base);
#else
extern "C" REX_FUNC(sub_8229F8B8) {
  __imp__sub_8229F8B8(ctx, base);
#endif
  const uint32_t session = REX_LOAD_U32(kLobby.lobby + 32);
  const std::array<uint32_t, 11> now = {
      ctx.r3.u32,
      PlayerCounted(base, kLobby.player_slots),
      PlayerCounted(base, kLobby.player_slots + 4),
      session,
      session ? REX_LOAD_U8(session + 104) : 0u,
      REX_LOAD_U32(kLobby.lobby),
      kLobby.tu_blocker ? REX_LOAD_U32(kLobby.tu_blocker) : 0u,
      REX_LOAD_U32(kLobby.required),
      REX_LOAD_U32(kLobby.remote_state),
      REX_LOAD_U32(kLobby.remote_word),
      REX_LOAD_U32(kLobby.start_blocker),
  };
  static std::array<uint32_t, 11> last = {~0u};
  if (now == last) return;
  last = now;
  REXLOG_INFO(
      "[kameo] lobby: host can start {} | players {}+{} of {} | session {:08X} flag {} | lobby state {} (needs 4) | "
      "remote state {} (needs 4) word {:08X} (needs non-zero) | blockers {} {} (need 0)",
      now[0], now[1], now[2], now[7], now[3], now[4], now[5], now[8], now[9], now[10], now[6]);
}
