// Names for downloadable Kameo skins, whose strings only the title update's string tables carry, and the Bonus costume
// a shader mod for her everyday outfit adds.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "core/guest.h"
#include "engine/dlc/dlc.h"

namespace {

struct KameoSkinName {
  const char* model;
  uint32_t string_id;
  const char16_t* english;
};

// The title update's skin table (17 entries at 0x82048920 in the patched image): the three built-in skins name themselves from vanilla strings, and the downloadable ones from strings only the update's own tables carry, so their English names are kept here.
constexpr KameoSkinName kKameoSkinNames[] = {
    {"007183BF_00", 1976, nullptr}, {"007183BF_01", 1977, nullptr}, {"007183BF_02", 1978, nullptr},
    {"007183BF_35", 4884, u"Armored Kameo"}, {"007183BF_46", 4883, u"Fright Pack Kameo"},
    {"007183BF_57", 4895, u"Masquerade Kameo #1"}, {"007183BF_58", 4896, u"Masquerade Kameo #2"}, {"007183BF_59", 4897, u"Masquerade Kameo #3"}, {"007183BF_60", 4898, u"Masquerade Kameo #4"}, {"007183BF_61", 4899, u"Masquerade Kameo #5"},
    {"007183BF_62", 4890, u"Masquerade Kameo #6"}, {"007183BF_63", 4891, u"Masquerade Kameo #7"}, {"007183BF_64", 4892, u"Masquerade Kameo #8"}, {"007183BF_65", 4893, u"Masquerade Kameo #9"}, {"007183BF_66", 4894, u"Masquerade Kameo #10"},
    {"007183BF_xmas1", 4882, u"Winter Warrior Kameo"}, {"007183BF_67", 35000, u"Design-A-Skin Kameo"},
};

}  // namespace

namespace kameo::engine {
namespace {

// objectAlternativeModelLists: each node links on at +4, keys its model type at +8 and points at the skin in use at
// +12, whose name starts at +51. A character's skins hang off a list head at +32, each linking on at +4.
constexpr uint32_t kAlternativeModelListFirst = 0x8280D104;
constexpr uint32_t kKameoModelType = 0x007183BF;
constexpr uint32_t kSkinNameBytes = 24;

// The characters a shader mod can make costumes for, by model type: Kameo, then the warriors.
constexpr uint32_t kCostumeModels[] = {kKameoModelType, 0x0CE65812, 0x0A70BAB2, 0x079B8925, 0x037AABBF, 0x005C444C, 0x08EBDF9E, 0x056D64DE, 0x05800519, 0x008B5284, 0x0BBEAEDE};

// Shader mod costumes: the string id of Kameo's first (ids no table carries, one per costume; the warriors' skin line
// names none), and the save suffix, "s" and six hex digits of a hash of the costume's name, so a save finds it again
// whatever order the files sort in. No model file has such a suffix, so a build without the costume loads the save in
// the character's standard look. The suffix fits the 8-byte buffer some of the game's callers read it into.
constexpr uint32_t kShaderSkinString = 35100;
constexpr uint32_t kShaderSkinSuffixLength = 7;
// The suffix the first build of this kept Kameo's one costume under; it loads as her first.
constexpr char kOldShaderSkinSuffix[] = "shader";
constexpr uint32_t kFirstShaderSkin = 0xFFFFFFFF;

struct CostumeSlot {
  // Guarded by g_shaderSkinMutex.
  std::vector<std::u16string> names;
  // The costume chosen, by CostumeId, or 0; it may name one that isn't installed, which then counts as none.
  uint32_t chosen_id = 0;
  // Its position in names, or -1: worked out under the mutex whenever either changes, read without it.
  std::atomic<int> chosen{-1};
  // No skin model loaded over the character's standard look.
  std::atomic<bool> standard{true};
};

std::mutex g_shaderSkinMutex;
CostumeSlot g_slots[std::size(kCostumeModels)];

CostumeSlot* SlotOf(uint32_t model_type) {
  for (size_t i = 0; i < std::size(kCostumeModels); ++i) {
    if (kCostumeModels[i] == model_type) return &g_slots[i];
  }
  return nullptr;
}

uint32_t CostumeId(const std::u16string& name) {
  uint32_t hash = 2166136261u;
  for (const char16_t c : name) hash = (hash ^ c) * 16777619u;
  hash &= 0xFFFFFF;
  return hash ? hash : 1;
}

// Call with g_shaderSkinMutex held.
void ChooseShaderSkin(CostumeSlot& slot, uint32_t id) {
  slot.chosen_id = id;
  int chosen = -1;
  for (size_t i = 0; id && i < slot.names.size() && chosen < 0; ++i) {
    if (id == kFirstShaderSkin || CostumeId(slot.names[i]) == id) chosen = int(i);
  }
  slot.chosen.store(chosen, std::memory_order_relaxed);
}

// Reads a save suffix as a shader mod costume, or 0 for any other.
uint32_t ShaderSkinOfSuffix(uint8_t* base, uint32_t suffix) {
  if (!suffix) return 0;
  char text[16] = {};
  for (uint32_t i = 0; i + 1 < sizeof(text); ++i) {
    text[i] = char(REX_LOAD_U8(suffix + i));
    if (!text[i]) break;
  }
  if (std::strcmp(text, kOldShaderSkinSuffix) == 0) return kFirstShaderSkin;
  if (std::strlen(text) != kShaderSkinSuffixLength || text[0] != 's') return 0;
  uint32_t id = 0;
  for (uint32_t i = 1; i < kShaderSkinSuffixLength; ++i) {
    const char c = text[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) return 0;
    id = id * 16 + uint32_t(digit);
  }
  return id;
}

// Stepping back a costume: the game only steps forward, from the costume in use to the next one unlocked, so the
// step back runs the game's own step with its find-next told where to land.
bool g_stepBack = false;
bool g_findNextOverride = false;
uint32_t g_findNextTarget = 0;

// A character's node in objectAlternativeModelLists, or 0.
uint32_t SkinNode(uint8_t* base, uint32_t model_type) {
  for (uint32_t node = REX_LOAD_U32(kAlternativeModelListFirst); node; node = REX_LOAD_U32(node + 4)) {
    if (REX_LOAD_U32(node + 8) == model_type) return node;
  }
  return 0;
}

// The skin models the game's step can reach, in its order: the ones objectAlternativeModelUnlockedCheck passes.
std::vector<uint32_t> UnlockedCostumes(PPCContext& ctx, uint8_t* base, uint32_t node) {
  std::vector<uint32_t> costumes;
  for (uint32_t entry = REX_LOAD_U32(node + 32 + 4); entry; entry = REX_LOAD_U32(entry + 4)) {
    PPCContext call = ctx;
    call.r3.u64 = entry + 51;
    sub_8224F458(call, base);
    if (call.r3.u32) costumes.push_back(entry);
  }
  return costumes;
}

std::string SkinInUse(uint8_t* base, uint32_t node) {
  const uint32_t entry = node ? REX_LOAD_U32(node + 12) : 0;
  std::string name;
  for (uint32_t i = 0; entry && i + 1 < kSkinNameBytes; ++i) {
    const char c = char(REX_LOAD_U8(entry + 51 + i));
    if (!c) break;
    name.push_back(c);
  }
  return name;
}

}  // namespace

void TickKameoSkin(uint8_t* base) {
  static std::string last = "?";
  for (size_t i = 0; i < std::size(kCostumeModels); ++i) {
    std::string skin = SkinInUse(base, SkinNode(base, kCostumeModels[i]));
    g_slots[i].standard.store(skin.empty(), std::memory_order_relaxed);
    if (kCostumeModels[i] != kKameoModelType || skin == last) continue;
    REXLOG_INFO("[kameo] skin in use: '{}'", skin);
    last = std::move(skin);
  }
}

void SetShaderSkins(std::vector<std::pair<uint32_t, std::u16string>> costumes) {
  std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
  for (CostumeSlot& slot : g_slots) slot.names.clear();
  for (auto& [model_type, name] : costumes) {
    if (CostumeSlot* slot = SlotOf(model_type)) slot->names.push_back(std::move(name));
  }
  for (CostumeSlot& slot : g_slots) ChooseShaderSkin(slot, slot.chosen_id);
}

int ShaderSkinChosen(uint32_t model_type) {
  const CostumeSlot* slot = SlotOf(model_type);
  return slot ? slot->chosen.load(std::memory_order_relaxed) : -1;
}

bool WearsShaderSkin(uint32_t model_type) {
  const CostumeSlot* slot = SlotOf(model_type);
  return slot && slot->chosen.load(std::memory_order_relaxed) >= 0 && slot->standard.load(std::memory_order_relaxed);
}

void SetCostumeStepBack(bool back) { g_stepBack = back; }

void SettingSkin(uint8_t* base, uint32_t model_type, PPCRegister& r4, PPCRegister& r1) {
  CostumeSlot* slot = SlotOf(model_type);
  if (!slot) return;
  const uint32_t id = ShaderSkinOfSuffix(base, r4.u32);
  {
    std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
    ChooseShaderSkin(*slot, id);
  }
  if (!id) return;
  // An empty suffix is the standard look. The same spot below the stack pointer as the skin hot-swap uses.
  const uint32_t empty = r1.u32 - 80;
  WriteGuestStringAt(base, empty, "");
  r4.u64 = empty;
}

}  // namespace kameo::engine

std::u16string KameoDlcSkinName(uint32_t string_id) {
  using namespace kameo::engine;
  if (string_id >= kShaderSkinString) {
    std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
    const std::vector<std::u16string>& names = SlotOf(kKameoModelType)->names;
    if (string_id - kShaderSkinString < names.size()) return names[string_id - kShaderSkinString];
  }
  for (const KameoSkinName& skin : kKameoSkinNames) {
    if (skin.string_id == string_id && skin.english) return skin.english;
  }
  return {};
}

// xfixgem_dlc_findAlternativeModelById(model type) -- a skin line's A press, Kameo's on the Bonus pages and a
// warrior's on its page, moves to the next skin unlocked, and from the last one back to the standard look. Shader mod
// costumes come between the last one and the standard look, in file name order: each is the standard look with its mod
// chosen, so no model loads for them. Stepping back runs the same order in reverse.
extern "C" REX_FUNC(sub_8224F530) {
  using namespace kameo::engine;
  const uint32_t model_type = ctx.r3.u32;
  CostumeSlot* slot = SlotOf(model_type);
  if (!slot) {
    __imp__sub_8224F530(ctx, base);
    return;
  }
  std::unique_lock<std::mutex> lock(g_shaderSkinMutex);
  const int count = int(slot->names.size());
  const int chosen = slot->chosen.load(std::memory_order_relaxed);
  if (g_stepBack) {
    const uint32_t node = SkinNode(base, model_type);
    if (!node) return;
    if (chosen > 0) {
      ChooseShaderSkin(*slot, CostumeId(slot->names[chosen - 1]));
      return;
    }
    const uint32_t current = REX_LOAD_U32(node + 12);
    // From the standard look, back to the last shader mod costume.
    if (chosen < 0 && !current && count > 0) {
      ChooseShaderSkin(*slot, CostumeId(slot->names[count - 1]));
      return;
    }
    ChooseShaderSkin(*slot, 0);
    lock.unlock();
    // Otherwise to the skin model before: the last one from the first shader mod costume or the standard look, and
    // the standard look (none) from the first.
    const std::vector<uint32_t> costumes = UnlockedCostumes(ctx, base, node);
    uint32_t target = 0;
    if (!current) {
      target = costumes.empty() ? 0 : costumes.back();
    } else if (const auto at = std::find(costumes.begin(), costumes.end(), current); at != costumes.end() && at != costumes.begin()) {
      target = *(at - 1);
    }
    if (!current && !target) return;
    g_findNextTarget = target;
    g_findNextOverride = true;
    __imp__sub_8224F530(ctx, base);
    g_findNextOverride = false;
    return;
  }
  if (chosen >= 0) {
    ChooseShaderSkin(*slot, chosen + 1 < count ? CostumeId(slot->names[chosen + 1]) : 0);
    return;
  }
  ChooseShaderSkin(*slot, 0);
  const uint32_t first = count ? CostumeId(slot->names[0]) : 0;
  lock.unlock();
  const uint32_t node = SkinNode(base, model_type);
  const bool from_skin = !SkinInUse(base, node).empty();
  __imp__sub_8224F530(ctx, base);
  if (!first || !from_skin || !SkinInUse(base, node).empty()) return;
  lock.lock();
  ChooseShaderSkin(*slot, first);
}

// objectAlternativeModelFindNext(node) -- moves the node's skin in use (+12) on to the next one unlocked, or none
// past the last. Only the next-skin call uses it; a step back has it land on the skin chosen. The game's step then
// loads that skin as it would the next, and unloads the one before.
extern "C" REX_FUNC(sub_8224F4D0) {
  using namespace kameo::engine;
  if (!g_findNextOverride || !ctx.r3.u32) {
    __imp__sub_8224F4D0(ctx, base);
    return;
  }
  REX_STORE_U32(ctx.r3.u32 + 12, g_findNextTarget);
  ctx.r3.u64 = 1;
}

// objectAlternativeGetFileInUse(model type, out suffix) -- what a save keeps for each model; a shader mod costume is
// kept by its own suffix, where the game would write the standard look's empty one.
extern "C" REX_FUNC(sub_82250130) {
  using namespace kameo::engine;
  const uint32_t model_type = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82250130(ctx, base);
  CostumeSlot* slot = SlotOf(model_type);
  if (!slot || !out || REX_LOAD_U8(out)) return;
  std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
  const int chosen = slot->chosen.load(std::memory_order_relaxed);
  if (chosen < 0) return;
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "s%06X", CostumeId(slot->names[chosen]));
  WriteGuestStringAt(base, out, suffix);
}

// storybookBonusMenuAddText(window, item, label, value, number, callback) -- createBonusTextOverlay names the Kameo skin in use from a three-entry table (007183BF_00, _01, _02) only when the skin's unlock event is done, and calls every other skin "Downloaded Kameo"; the skin in use is named from the title update's fuller table instead.
extern "C" REX_FUNC(sub_823C9D78) {
  constexpr uint32_t kBonusKameoSkinItem = 159937678;
  constexpr uint32_t kDownloadedKameoString = 4673;
  constexpr uint32_t kKameoModelType = 0x007183BF;
  // objectAlternativeModelLists: each node links on at +4, keys its model type at +8 and points at the skin in use at +12, whose name starts at +51.
  constexpr uint32_t kAlternativeModelListFirst = 0x8280D104;
  if (const int chosen = kameo::engine::ShaderSkinChosen(kKameoModelType); ctx.r4.u32 == kBonusKameoSkinItem && chosen >= 0) {
    ctx.r6.u64 = kameo::engine::kShaderSkinString + uint32_t(chosen);
  } else if (ctx.r4.u32 == kBonusKameoSkinItem && ctx.r6.u32 == kDownloadedKameoString) {
    for (uint32_t node = REX_LOAD_U32(kAlternativeModelListFirst); node; node = REX_LOAD_U32(node + 4)) {
      if (REX_LOAD_U32(node + 8) != kKameoModelType) continue;
      const uint32_t entry = REX_LOAD_U32(node + 12);
      if (!entry) break;
      char name[24] = {};
      for (uint32_t i = 0; i + 1 < sizeof(name); ++i) {
        name[i] = char(REX_LOAD_U8(entry + 51 + i));
        if (!name[i]) break;
      }
      for (const KameoSkinName& skin : kKameoSkinNames) {
        if (std::strcmp(name, skin.model) == 0) ctx.r6.u64 = skin.string_id;
      }
      break;
    }
  }
  __imp__sub_823C9D78(ctx, base);
}
