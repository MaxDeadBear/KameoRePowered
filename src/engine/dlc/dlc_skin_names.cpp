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

// objectAlternativeModelLists: each node links on at +4, keys its model type at +8 and points at the skin in use at +12, whose name starts at +51.
constexpr uint32_t kAlternativeModelListFirst = 0x8280D104;
constexpr uint32_t kKameoModelType = 0x007183BF;
constexpr uint32_t kSkinNameBytes = 24;

// Shader mod costumes: the string id of the first (ids no table carries, one per costume), and the save suffix, "s"
// and six hex digits of a hash of the costume's name, so a save finds it again whatever order the files sort in. No
// model file has such a suffix, so a build without the costume loads the save in her everyday outfit. The suffix fits
// the 8-byte buffer some of the game's callers read it into.
constexpr uint32_t kShaderSkinString = 35100;
constexpr uint32_t kShaderSkinSuffixLength = 7;
// The suffix the first build of this kept the one costume under; it loads as the first costume.
constexpr char kOldShaderSkinSuffix[] = "shader";
constexpr uint32_t kFirstShaderSkin = 0xFFFFFFFF;

std::atomic<bool> g_defaultSkin{true};
std::mutex g_shaderSkinMutex;
std::vector<std::u16string> g_shaderSkinNames;
// The costume chosen, by CostumeId, or 0; it may name one that isn't installed, which then counts as none.
uint32_t g_shaderSkinChosenId = 0;
// Its position in g_shaderSkinNames, or -1: worked out under the mutex whenever either changes, read without it.
std::atomic<int> g_shaderSkinChosen{-1};

uint32_t CostumeId(const std::u16string& name) {
  uint32_t hash = 2166136261u;
  for (const char16_t c : name) hash = (hash ^ c) * 16777619u;
  hash &= 0xFFFFFF;
  return hash ? hash : 1;
}

// Call with g_shaderSkinMutex held.
void ChooseShaderSkin(uint32_t id) {
  g_shaderSkinChosenId = id;
  int chosen = -1;
  for (size_t i = 0; id && i < g_shaderSkinNames.size() && chosen < 0; ++i) {
    if (id == kFirstShaderSkin || CostumeId(g_shaderSkinNames[i]) == id) chosen = int(i);
  }
  g_shaderSkinChosen.store(chosen, std::memory_order_relaxed);
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

// Kameo's node in objectAlternativeModelLists. Its costumes hang off a list head at +32, each linking on at +4.
uint32_t KameoSkinNode(uint8_t* base) {
  for (uint32_t node = REX_LOAD_U32(kAlternativeModelListFirst); node; node = REX_LOAD_U32(node + 4)) {
    if (REX_LOAD_U32(node + 8) == kKameoModelType) return node;
  }
  return 0;
}

// Her costume models the game's step can reach, in its order: the ones objectAlternativeModelUnlockedCheck passes.
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

std::string KameoSkinInUse(uint8_t* base) {
  for (uint32_t node = REX_LOAD_U32(kAlternativeModelListFirst); node; node = REX_LOAD_U32(node + 4)) {
    if (REX_LOAD_U32(node + 8) != kKameoModelType) continue;
    const uint32_t entry = REX_LOAD_U32(node + 12);
    std::string name;
    for (uint32_t i = 0; entry && i + 1 < kSkinNameBytes; ++i) {
      const char c = char(REX_LOAD_U8(entry + 51 + i));
      if (!c) break;
      name.push_back(c);
    }
    return name;
  }
  return {};
}

}  // namespace

void TickKameoSkin(uint8_t* base) {
  static std::string last = "?";
  std::string skin = KameoSkinInUse(base);
  if (skin == last) return;
  REXLOG_INFO("[kameo] skin in use: '{}'", skin);
  g_defaultSkin.store(skin.empty(), std::memory_order_relaxed);
  last = std::move(skin);
}

void SetKameoShaderSkins(std::vector<std::u16string> names) {
  std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
  g_shaderSkinNames = std::move(names);
  ChooseShaderSkin(g_shaderSkinChosenId);
}

int KameoShaderSkinChosen() { return g_shaderSkinChosen.load(std::memory_order_relaxed); }

bool KameoWearsShaderSkin() { return KameoShaderSkinChosen() >= 0 && g_defaultSkin.load(std::memory_order_relaxed); }

void SetKameoCostumeStepBack(bool back) { g_stepBack = back; }

void KameoSettingSkin(uint8_t* base, PPCRegister& r4, PPCRegister& r1) {
  const uint32_t id = ShaderSkinOfSuffix(base, r4.u32);
  {
    std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
    ChooseShaderSkin(id);
  }
  if (!id) return;
  // An empty suffix is her everyday outfit. The same spot below the stack pointer as the skin hot-swap uses.
  const uint32_t empty = r1.u32 - 80;
  WriteGuestStringAt(base, empty, "");
  r4.u64 = empty;
}

}  // namespace kameo::engine

std::u16string KameoDlcSkinName(uint32_t string_id) {
  using namespace kameo::engine;
  if (string_id >= kShaderSkinString) {
    std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
    if (string_id - kShaderSkinString < g_shaderSkinNames.size()) return g_shaderSkinNames[string_id - kShaderSkinString];
  }
  for (const KameoSkinName& skin : kKameoSkinNames) {
    if (skin.string_id == string_id && skin.english) return skin.english;
  }
  return {};
}

// xfixgem_dlc_findAlternativeModelById(model type) -- the Bonus costume line's A press moves to the next costume
// unlocked, and from the last one back to her everyday outfit. Shader mod costumes come between the last one and her
// everyday outfit, in file name order: each is the everyday outfit with its mod chosen, so no model loads for them.
// Stepping back runs the same order in reverse.
extern "C" REX_FUNC(sub_8224F530) {
  using namespace kameo::engine;
  std::unique_lock<std::mutex> lock(g_shaderSkinMutex);
  const int count = int(g_shaderSkinNames.size());
  if (ctx.r3.u32 == kKameoModelType && g_stepBack) {
    const uint32_t node = KameoSkinNode(base);
    if (!node) return;
    const int chosen = KameoShaderSkinChosen();
    if (chosen > 0) {
      ChooseShaderSkin(CostumeId(g_shaderSkinNames[chosen - 1]));
      return;
    }
    const uint32_t current = REX_LOAD_U32(node + 12);
    // From her everyday outfit, back to the last shader mod costume.
    if (chosen < 0 && !current && count > 0) {
      ChooseShaderSkin(CostumeId(g_shaderSkinNames[count - 1]));
      return;
    }
    ChooseShaderSkin(0);
    lock.unlock();
    // Otherwise to the costume model before: the last one from the first shader mod costume or her everyday outfit,
    // and her everyday outfit (none) from the first.
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
  if (ctx.r3.u32 != kKameoModelType || count == 0) {
    if (ctx.r3.u32 == kKameoModelType) ChooseShaderSkin(0);
    lock.unlock();
    __imp__sub_8224F530(ctx, base);
    return;
  }
  if (const int chosen = KameoShaderSkinChosen(); chosen >= 0) {
    ChooseShaderSkin(chosen + 1 < count ? CostumeId(g_shaderSkinNames[chosen + 1]) : 0);
    return;
  }
  ChooseShaderSkin(0);
  const uint32_t first = CostumeId(g_shaderSkinNames[0]);
  lock.unlock();
  const bool from_costume = !KameoSkinInUse(base).empty();
  __imp__sub_8224F530(ctx, base);
  if (!from_costume || !KameoSkinInUse(base).empty()) return;
  lock.lock();
  ChooseShaderSkin(first);
}

// objectAlternativeModelFindNext(node) -- moves the node's costume in use (+12) on to the next one unlocked, or none
// past the last. Only the next-costume call uses it; a step back has it land on the costume chosen. The game's step
// then loads that costume as it would the next, and unloads the one before.
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
// kept by its own suffix, where the game would write her everyday outfit's empty one.
extern "C" REX_FUNC(sub_82250130) {
  using namespace kameo::engine;
  const uint32_t type = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82250130(ctx, base);
  if (type != kKameoModelType || !out || REX_LOAD_U8(out)) return;
  std::lock_guard<std::mutex> lock(g_shaderSkinMutex);
  const int chosen = KameoShaderSkinChosen();
  if (chosen < 0) return;
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "s%06X", CostumeId(g_shaderSkinNames[chosen]));
  WriteGuestStringAt(base, out, suffix);
}

// storybookBonusMenuAddText(window, item, label, value, number, callback) -- createBonusTextOverlay names the Kameo skin in use from a three-entry table (007183BF_00, _01, _02) only when the skin's unlock event is done, and calls every other skin "Downloaded Kameo"; the skin in use is named from the title update's fuller table instead.
extern "C" REX_FUNC(sub_823C9D78) {
  constexpr uint32_t kBonusKameoSkinItem = 159937678;
  constexpr uint32_t kDownloadedKameoString = 4673;
  constexpr uint32_t kKameoModelType = 0x007183BF;
  // objectAlternativeModelLists: each node links on at +4, keys its model type at +8 and points at the skin in use at +12, whose name starts at +51.
  constexpr uint32_t kAlternativeModelListFirst = 0x8280D104;
  if (const int chosen = kameo::engine::KameoShaderSkinChosen(); ctx.r4.u32 == kBonusKameoSkinItem && chosen >= 0) {
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
