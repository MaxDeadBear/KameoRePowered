// Player 2 shares player 1's warrior wheel: MorphValid hands us each player's sack, and player 1's slot assignments are mirrored into player 2's.

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "core/hooks.h"
#include "engine/splitscreen_internal.h"

// Player 2's wheel already opens and sends the morph; MorphValid refuses it only because his sack's slots read FFFF.
REXCVAR_DEFINE_BOOL(kameo_player2_morph, true, "Kameo",
                    "Give player 2 the same warriors player 1 has mapped.");

namespace kameo::engine {
namespace {

// The character object whose [2680] is valid is the player_list entry + 0x200 (p1 41F8B2B0 -> 41F8B4B0, p2 4203BC00 -> 4203BE00).
constexpr uint32_t kCharacterObjectOffset = 0x200;
// MorphValid: data = *(obj + 2680); sack = *(data + 548); warriorIdx = *(u16*)(sack + 2 * (slot + 4468)), FFFF refuses.
constexpr uint32_t kCharacterData = 2680;
constexpr uint32_t kDataSack = 548;
constexpr uint32_t kSackSlotBase = 4468;

// Sacks captured from MorphValid's own receiver; deriving them from player_list every frame kept faulting on transient garbage.
std::atomic<uint32_t> g_sack_p1{0};
std::atomic<uint32_t> g_sack_p2{0};

// Sacks live in the image's static data (0x82804058, 0x82806358); a looser check let the float 1.0 (0x3F800000) through during the battlefield transition.
bool PlausibleSackPtr(uint32_t v) {
  return v >= 0x82000000u && v < 0x82C00000u;
}

}  // namespace

void SyncPlayer2Warriors(uint8_t* base) {
  if (!REXCVAR_GET(kameo_player2_morph)) {
    return;
  }
  const uint32_t sack1 = g_sack_p1.load(std::memory_order_acquire);
  const uint32_t sack2 = g_sack_p2.load(std::memory_order_acquire);
  if (!PlausibleSackPtr(sack1) || !PlausibleSackPtr(sack2) || sack1 == sack2) {
    return;  // not captured yet, or not both players
  }

  bool changed = false;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint32_t off = 2 * (slot + kSackSlotBase);
    const uint16_t src = static_cast<uint16_t>(REX_LOAD_U16(sack1 + off));
    const uint16_t dst = static_cast<uint16_t>(REX_LOAD_U16(sack2 + off));
    if (src != dst) {
      REX_STORE_U16(sack2 + off, src);
      changed = true;
    }
  }
}

}  // namespace kameo::engine

using namespace kameo::engine;

// Entry of MorphValid(obj, slot): records each player's sack.
void KameoCaptureWarriorSack(PPCRegister& r3) {
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  const uint32_t obj = r3.u32;
  const uint32_t data = obj ? REX_LOAD_U32(obj + kCharacterData) : 0;
  const uint32_t sack = data ? REX_LOAD_U32(data + kDataSack) : 0;
  // r3 comes from the game, so this chain is safe here even though deriving it from player_list every frame was not.
  if (PlausibleSackPtr(sack)) {
    const uint32_t p1 = REX_LOAD_U32(kPlayerList + 0);
    const uint32_t p2 = REX_LOAD_U32(kPlayerList + 4);
    if (p1 != 0 && obj == p1 + kCharacterObjectOffset) {
      g_sack_p1.store(sack, std::memory_order_release);
    } else if (p2 != 0 && obj == p2 + kCharacterObjectOffset) {
      g_sack_p2.store(sack, std::memory_order_release);
    }
  }
}
