// The game's collectibles, each recorded by one saved event: which are found, which a sequence break left behind, and granting one.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/guest.h"

namespace kameo::engine {

enum class CollectibleKind : uint8_t { Fruit, Elixir, CrystalEye, Costume };
// Where the item is got: placed in the level, sold by its tribe shop, or won in a warrior trainer's game.
enum class CollectibleSource : uint8_t { Level, Shop, Trainer };

struct Collectible {
  CollectibleKind kind;
  // ELF hash of the level's name, the name of its .lvl file.
  uint32_t level;
  uint32_t event;
  // Elemental fruit come as singles and as triples; the game's total of 100 counts them by value.
  uint8_t value;
  CollectibleSource source;
  // The last story task set in the level, or -1 for a side area that stays reachable.
  int16_t last_task;
  // A costume's warrior, whose story event must be done before the costume means anything; -1 otherwise.
  int32_t warrior_event;
};

extern const Collectible kCollectibles[];
extern const size_t kCollectibleCount;

uint32_t CurrentLevelToken();
bool CollectibleFound(PPCContext& ctx, uint8_t* base, const Collectible& item);
// Not found, placed in a story level whose last task is complete, and for a costume, its warrior owned.
bool CollectibleMissed(PPCContext& ctx, uint8_t* base, const Collectible& item);
// Sets the item's event the way picking it up does, without the score a pickup awards, and saves.
void GrantCollectible(PPCContext& ctx, uint8_t* base, const Collectible& item);

}  // namespace kameo::engine
