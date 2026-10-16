// Names for downloadable Kameo skins, whose strings only the title update's string tables carry.

#include <cstring>

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

const char16_t* KameoDlcSkinName(uint32_t string_id) {
  for (const KameoSkinName& skin : kKameoSkinNames) {
    if (skin.string_id == string_id) return skin.english;
  }
  return nullptr;
}

// storybookBonusMenuAddText(window, item, label, value, number, callback) -- createBonusTextOverlay names the Kameo skin in use from a three-entry table (007183BF_00, _01, _02) only when the skin's unlock event is done, and calls every other skin "Downloaded Kameo"; the skin in use is named from the title update's fuller table instead.
extern "C" REX_FUNC(sub_823C9D78) {
  constexpr uint32_t kBonusKameoSkinItem = 159937678;
  constexpr uint32_t kDownloadedKameoString = 4673;
  constexpr uint32_t kKameoModelType = 0x007183BF;
  // objectAlternativeModelLists: each node links on at +4, keys its model type at +8 and points at the skin in use at +12, whose name starts at +51.
  constexpr uint32_t kAlternativeModelListFirst = 0x8280D104;
  if (ctx.r4.u32 == kBonusKameoSkinItem && ctx.r6.u32 == kDownloadedKameoString) {
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
