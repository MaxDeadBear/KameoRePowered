// Online co-op in the title update: the joiner's Kameo wears a skin of the joiner's own choosing.
//
// The game keeps one skin per model type: a skin's .mdl is loaded as a database level, and its model asset goes into a
// tree keyed by model type (objectAlternativeModelLists' tree at 0x82868334). Each object builds its own model from
// that asset in objectAlternativeModelCheck, so both players' Kameos look alike, and the update's sync pushes the
// host's choice to the joiner. Here the joiner's skin is loaded as a level of its own (or the host's copy is borrowed
// when both pick the same one), and the joiner's Kameo alone builds its models from it, on both consoles. A skin level
// holds more than Kameo's body (her wings are objects of their own, with their own model type), so every model in it
// is handled alike. The joiner's Bonus page costume line steps this skin, and each choice goes to the host in the
// update's own skin-sync message, tagged so only this code reads it.

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "core/guest.h"
#include "core/localization.h"
#include "engine/dlc/dlc.h"
#include "kameorepowered_init.h"

#if KAMEO_TU

namespace {

constexpr uint32_t kGameState = 0x82BCEE80;  // 2 while a level runs
constexpr uint32_t kLobbyState = 0x82BCF18C;
constexpr uint32_t kSession = 0x82BCF1AC;    // its byte +104 is set on the host
constexpr uint32_t kSkinNodes = 0x82868330;  // objectAlternativeModelLists' first node
constexpr uint32_t kSkinTree = 0x82868334;   // model type -> loaded skin asset (+16 key, +20 asset, +24 record)
constexpr uint32_t kSkinTreeNil = 0x82903F6C;
constexpr uint32_t kKameoModelType = 0x007183BF;
constexpr uint32_t kModelAssetType = 0x0012501C;
constexpr uint32_t kMdlExtension = 0x8203D194;  // ".mdl"
constexpr uint32_t kDatabaseLock = 0x828EB274;
constexpr uint32_t kBonusKameoSkinItem = 159937678;
// Marks a skin-sync message as the joiner's own skin; the update's sync never sets this bit of a model type.
constexpr uint32_t kJoinerSkinTag = 0x80000000;

struct JoinerSkin {
  std::string suffix;     // "" is Kameo's standard look
  uint32_t record = 0;    // its skin record in Kameo's list, or 0
  uint32_t loaded = 0;    // a record whose level this code loaded (or kept from being freed)
  // The level's model assets by model type, empty while loading or for the standard look.
  std::unordered_map<uint32_t, uint32_t> assets;
};
JoinerSkin g_skin;
struct Applied {
  uint32_t asset;
  uint32_t record;  // the skin whose level the asset belongs to
};
std::unordered_map<uint32_t, Applied> g_applied;  // object -> what its model was built from
uint32_t g_guestSuffix = 0;

uint32_t Call(PPCContext& ctx, uint8_t* base, void (*f)(PPCContext&, uint8_t*), uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0) {
  PPCContext call = ctx;
  call.r3.u64 = r3;
  call.r4.u64 = r4;
  call.r5.u64 = r5;
  call.r6.u64 = r6;
  call.r7.u64 = r7;
  f(call, base);
  return call.r3.u32;
}

std::string GuestText(uint8_t* base, uint32_t address) {
  std::string text;
  for (uint32_t i = 0; address && i < 48; ++i) {
    const char c = char(REX_LOAD_U8(address + i));
    if (!c) break;
    text.push_back(c);
  }
  return text;
}

bool NetGame(uint8_t* base) { return REX_LOAD_U32(kLobbyState) != 0 && REX_LOAD_U32(kSession) != 0; }
bool IsHost(uint8_t* base) {
  const uint32_t session = REX_LOAD_U32(kSession);
  return session && REX_LOAD_U8(session + 104) != 0;
}

uint32_t KameoNode(uint8_t* base) {
  for (uint32_t node = REX_LOAD_U32(kSkinNodes); node; node = REX_LOAD_U32(node + 4)) {
    if (REX_LOAD_U32(node + 8) == kKameoModelType) return node;
  }
  return 0;
}

uint32_t FindRecord(uint8_t* base, const std::string& suffix) {
  const uint32_t node = KameoNode(base);
  const std::string name = "007183BF_" + suffix;
  for (uint32_t entry = node ? REX_LOAD_U32(node + 32 + 4) : 0; entry; entry = REX_LOAD_U32(entry + 4)) {
    if (GuestText(base, entry + 51) == name) return entry;
  }
  return 0;
}

// The tree's root, as objectAlternativeModelCheck finds it, or 0 when no skin is loaded.
uint32_t TreeRoot(uint8_t* base) {
  const uint32_t nil = REX_LOAD_U32(kSkinTreeNil);
  uint32_t node = REX_LOAD_U32(kSkinTree);
  if (node == nil) return 0;
  while (REX_LOAD_U32(node) != nil) node = REX_LOAD_U32(node);
  return node;
}

// The shared skin's tree node for a model type, or 0.
uint32_t TreeNode(uint8_t* base, uint32_t model_type) {
  const uint32_t nil = REX_LOAD_U32(kSkinTreeNil);
  for (uint32_t node = TreeRoot(base); node && node != nil;) {
    const int32_t diff = int32_t(model_type - REX_LOAD_U32(node + 16));
    if (diff == 0) return node;
    node = REX_LOAD_U32(node + (diff < 0 ? 4 : 8));
  }
  return 0;
}

uint32_t SharedNode(uint8_t* base) { return TreeNode(base, kKameoModelType); }

// The shared skin's assets that came from a record's level, by model type.
void TreeAssetsOf(uint8_t* base, uint32_t node, uint32_t record, std::unordered_map<uint32_t, uint32_t>& out, int depth = 0) {
  if (!node || node == REX_LOAD_U32(kSkinTreeNil) || depth > 64) return;
  if (REX_LOAD_U32(node + 24) == record) out[REX_LOAD_U32(node + 16)] = REX_LOAD_U32(node + 20);
  TreeAssetsOf(base, REX_LOAD_U32(node + 4), record, out, depth + 1);
  TreeAssetsOf(base, REX_LOAD_U32(node + 8), record, out, depth + 1);
}

// The model assets in a loaded skin level by model type, as the skin-swap tick reads them; empty while it is loading.
std::unordered_map<uint32_t, uint32_t> AssetsOfLevel(PPCContext& ctx, uint8_t* base, uint32_t record) {
  const uint32_t name = record + 51;
  const int32_t count = int32_t(Call(ctx, base, sub_822EDAC8, name));
  std::unordered_map<uint32_t, uint32_t> found;
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t entry = Call(ctx, base, sub_822EDB58, name, uint32_t(i));
    if (!entry) continue;
    const int32_t assets = int32_t(REX_LOAD_U32(entry + 68));
    const uint32_t list = REX_LOAD_U32(entry + 72);
    for (int32_t k = 0; k < assets; ++k) {
      const uint32_t descriptor = REX_LOAD_U32(list + 4 * k);
      if (REX_LOAD_U32(descriptor) == kModelAssetType) found.emplace(REX_LOAD_U32(descriptor + 4), REX_LOAD_U32(REX_LOAD_U32(list + 4 * k + 4) + 8));
    }
    Call(ctx, base, __imp__RtlEnterCriticalSection, kDatabaseLock);
    Call(ctx, base, sub_820FCD58, entry);  // mmFree
    Call(ctx, base, __imp__RtlLeaveCriticalSection, kDatabaseLock);
  }
  return found;
}

bool LevelInUse(uint32_t record) {
  for (const auto& [object, applied] : g_applied) {
    if (applied.record == record) return true;
  }
  return false;
}

// A record in Kameo's skin list (named after her model type).
bool KameoSkinRecord(uint8_t* base, uint32_t record) {
  return record && GuestText(base, record + 51).rfind("007183BF_", 0) == 0;
}

// Frees a level this code holds once nothing is built from it and the shared skin isn't wearing it.
void ReleaseLoaded(PPCContext& ctx, uint8_t* base) {
  if (!g_skin.loaded || g_skin.loaded == g_skin.record) return;
  const uint32_t shared = SharedNode(base);
  const uint32_t shared_record = shared ? REX_LOAD_U32(shared + 24) : 0;
  if (shared_record == g_skin.loaded) {
    g_skin.loaded = 0;  // the shared skin owns it again
    return;
  }
  if (LevelInUse(g_skin.loaded)) return;
  REXLOG_INFO("[kameo] joiner skin: freeing '{}'", GuestText(base, g_skin.loaded + 51));
  Call(ctx, base, __imp__sub_822ED738, g_skin.loaded + 51, 0, 0);  // databaseFreeLevel, past the hook below
  g_skin.loaded = 0;
}

// Keeps g_skin.assets in step with the joiner's choice, loading its level when no one has.
void UpdateAsset(PPCContext& ctx, uint8_t* base) {
  // Read once per load: the level's object entries are allocated for each call.
  static uint32_t read_record = 0;
  static std::unordered_map<uint32_t, uint32_t> read_assets;
  if (g_skin.suffix.empty()) {
    g_skin.record = 0;
    g_skin.assets.clear();
    ReleaseLoaded(ctx, base);
    return;
  }
  if (!g_skin.record) g_skin.record = FindRecord(base, g_skin.suffix);
  if (!g_skin.record) {
    g_skin.assets.clear();
    return;
  }
  const uint32_t shared = SharedNode(base);
  if (shared && REX_LOAD_U32(shared + 24) == g_skin.record) {
    g_skin.assets.clear();
    TreeAssetsOf(base, TreeRoot(base), g_skin.record, g_skin.assets);
  } else if (int32_t(Call(ctx, base, sub_822EDAC8, g_skin.record + 51)) != 0) {
    if (read_record != g_skin.record || read_assets.empty()) {
      read_record = g_skin.record;
      read_assets = AssetsOfLevel(ctx, base, g_skin.record);
      for (const auto& [model_type, asset] : read_assets) {
        REXLOG_INFO("[kameo] joiner skin: '{}' holds model {:08X}", GuestText(base, g_skin.record + 51), model_type);
      }
    }
    g_skin.assets = read_assets;
  } else {
    g_skin.assets.clear();
    read_assets.clear();
    if (g_skin.loaded != g_skin.record) {
      ReleaseLoaded(ctx, base);
      if (!g_skin.loaded) {
        REXLOG_INFO("[kameo] joiner skin: loading '{}'", GuestText(base, g_skin.record + 51));
        Call(ctx, base, sub_822ED440, g_skin.record + 8, g_skin.record + 51, kMdlExtension, 0);  // databaseLoadLevel
        g_skin.loaded = g_skin.record;
      }
    }
  }
  if (!g_skin.assets.empty()) ReleaseLoaded(ctx, base);
}

// The object's own alternative model, by its class, as objectAlternativeModelCheck reads it.
uint32_t AlternativeModel(uint8_t* base, uint32_t object) {
  const uint32_t cls = REX_LOAD_U32(REX_LOAD_U32(object) + 44);
  const uint32_t data = REX_LOAD_U32(REX_LOAD_U32(object + 4) + 16);
  switch (cls) {
    case 0x06A010EB: return REX_LOAD_U32(data + 268);
    case 0x0D83C5EA: return REX_LOAD_U32(data + 180);
    case 0x0067AB62: return REX_LOAD_U32(data + 144);
    case 0x0590A8D1: return REX_LOAD_U32(data + 248);
    default: return 0;
  }
}

void ReregisterShadow(PPCContext& ctx, uint8_t* base, uint32_t base_model, uint32_t model) {
  const int8_t shadow = int8_t(REX_LOAD_U8(base_model + 464));
  if (shadow != -1) Call(ctx, base, sub_82270398, uint32_t(int32_t(shadow)), REX_LOAD_U32(REX_LOAD_U32(model + 64) + 28));
}

void FreeAlternative(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t base_model, uint32_t alternative) {
  Call(ctx, base, sub_8225DCE8, object, 0);  // objectSetAlternativeModel
  ReregisterShadow(ctx, base, base_model, base_model);
  Call(ctx, base, sub_8225A860, alternative);  // modelFree
}

void KeepAlternative(PPCContext& ctx, uint8_t* base, uint32_t base_model, uint32_t alternative) {
  Call(ctx, base, sub_8225BD30, base_model, alternative);  // modelForwardOnPartVisible
  REX_STORE_U8(alternative + 465, uint8_t((REX_LOAD_U8(alternative + 465) & 0xFC) | (REX_LOAD_U8(base_model + 465) & 3)));
  if (const uint32_t blend = REX_LOAD_U32(base_model + 316)) Call(ctx, base, sub_822A3330, blend, base_model, alternative);
}

// The joiner's next (or previous) skin, in the order the game's own step uses: the standard look, then each unlocked
// skin in the list.
std::string StepSuffix(PPCContext& ctx, uint8_t* base, bool back) {
  std::vector<std::string> order{""};
  const uint32_t node = KameoNode(base);
  for (uint32_t entry = node ? REX_LOAD_U32(node + 32 + 4) : 0; entry; entry = REX_LOAD_U32(entry + 4)) {
    if (!Call(ctx, base, sub_8225E158, entry)) continue;  // objectAlternativeModelUnlockedCheck
    const std::string name = GuestText(base, entry + 51);
    if (name.rfind("007183BF_", 0) == 0) order.push_back(name.substr(9));
  }
  size_t at = 0;
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == g_skin.suffix) at = i;
  }
  at = back ? (at + order.size() - 1) % order.size() : (at + 1) % order.size();
  return order[at];
}

void Choose(const std::string& suffix) {
  if (suffix == g_skin.suffix) return;
  g_skin.suffix = suffix;
  g_skin.record = 0;
  REXLOG_INFO("[kameo] joiner skin: '{}'", suffix);
}

void SendChoice(PPCContext& ctx, uint8_t* base) {
  if (!g_guestSuffix) g_guestSuffix = rex::system::kernel_state()->memory()->SystemHeapAlloc(64);
  if (!g_guestSuffix) return;
  WriteGuestStringAt(base, g_guestSuffix, g_skin.suffix.c_str());
  // The update's skin-sync send (kind, model type, suffix, have, flag).
  Call(ctx, base, sub_822B32C0, 0, kKameoModelType | kJoinerSkinTag, g_guestSuffix, 1, 0);
}

}  // namespace

bool kameo::engine::HandleJoinerSkinMessage(uint8_t* base, uint32_t message) {
  if (!message || (REX_LOAD_U32(message + 48) & kJoinerSkinTag) == 0) return false;
  Choose(GuestText(base, message + 52));
  return true;
}

// The shared skin record each other Kameo part's model was built under (see below).
static std::unordered_map<uint32_t, uint32_t> g_sharedBuilt;

// objectAlternativeModelCheck(gameObject) -- the joiner's Kameo (body and wings) builds its models from the joiner's
// skin.
extern "C" REX_FUNC(sub_8225E838) {
  const uint32_t game_object = ctx.r3.u32;
  const uint32_t object = game_object ? REX_LOAD_U32(game_object + 4) : 0;
  if (!object || REX_LOAD_U32(kGameState) != 2 || !NetGame(base)) {
    __imp__sub_8225E838(ctx, base);
    return;
  }
  const uint32_t base_model = Call(ctx, base, sub_82262690, object);  // objectGetModel
  if (!base_model) {
    __imp__sub_8225E838(ctx, base);
    return;
  }
  const uint32_t model_type = REX_LOAD_U32(REX_LOAD_U32(base_model + 64) + 4);
  const uint32_t shared = TreeNode(base, model_type);
  const uint32_t record = shared ? REX_LOAD_U32(shared + 24) : 0;
  // A part of Kameo's skins: her body, or another model her skin levels hold (the wings).
  const bool part = model_type == kKameoModelType || KameoSkinRecord(base, record) || g_skin.assets.count(model_type) || g_applied.count(object);
  if (!part) {
    __imp__sub_8225E838(ctx, base);
    return;
  }
  // +146 is the player number counted from each console's own player, the update's own skin check reads it for every
  // object: 0 is the local player's, so the joiner's is 1 on the host and 0 on the joiner. The joiner's Kameo never
  // follows the host's skin; until the joiner picks one it wears the standard look on both.
  const bool joiners = REX_LOAD_U8(object + 146) == (IsHost(base) ? 1 : 0);
  static std::unordered_map<uint32_t, uint32_t> logged;
  if (!logged.count(object) || logged[object] != model_type) {
    logged[object] = model_type;
    REXLOG_INFO("[kameo] joiner skin: {:08X} model {:08X} player {} on the {} is {}", object, model_type, REX_LOAD_U8(object + 146),
                IsHost(base) ? "host" : "joiner", joiners ? "the joiner's" : "the host's");
  }
  if (!joiners) {
    // The game keeps an object's model while the skin record in use matches the shared tree's, without asking which
    // level the model came from. A skin change normally passes through a moment with no tree entry (the next level
    // still loading), when objects drop the old model; when the joiner already has the next skin loaded, its entry
    // appears at once and the old model was kept until its level was freed under it. So a model built under another
    // record is dropped here first, and the game builds the current one.
    const uint32_t before = AlternativeModel(base, object);
    const auto built = g_sharedBuilt.find(object);
    if (before && record && built != g_sharedBuilt.end() && built->second != record) FreeAlternative(ctx, base, object, base_model, before);
    __imp__sub_8225E838(ctx, base);
    if (AlternativeModel(base, object)) g_sharedBuilt[object] = record;
    else g_sharedBuilt.erase(object);
    return;
  }
  UpdateAsset(ctx, base);
  const auto found = g_skin.assets.find(model_type);
  const uint32_t asset = found != g_skin.assets.end() ? found->second : 0;
  const uint32_t alternative = AlternativeModel(base, object);
  if (!asset) {
    if (alternative) FreeAlternative(ctx, base, object, base_model, alternative);
    g_applied.erase(object);
    return;
  }
  const auto applied = g_applied.find(object);
  if (alternative && applied != g_applied.end() && applied->second.asset == asset) {
    KeepAlternative(ctx, base, base_model, alternative);
    return;
  }
  if (alternative) FreeAlternative(ctx, base, object, base_model, alternative);
  const uint32_t model = Call(ctx, base, sub_8225A240, object, asset);  // modelInitByAsset
  Call(ctx, base, sub_8225DCE8, object, model);
  if (!model) {
    g_applied.erase(object);
    return;
  }
  g_applied[object] = {asset, g_skin.record};
  // As objectAlternativeModelCheck links a new model to its base model's animation.
  const uint32_t parts = REX_LOAD_U32(model + 304);
  REX_STORE_U32(REX_LOAD_U32(parts + 12) + REX_LOAD_U32(model + 68) + 4, REX_LOAD_U32(base_model + 308));
  ReregisterShadow(ctx, base, base_model, model);
  KeepAlternative(ctx, base, base_model, model);
}

// databaseFreeLevel(name, ...) -- a skin level the joiner's Kameo is still built from stays loaded; it is freed here
// once it isn't.
extern "C" REX_FUNC(sub_822ED738) {
  if (g_skin.record && ctx.r3.u32 && GuestText(base, ctx.r3.u32) == GuestText(base, g_skin.record + 51) && !g_skin.assets.empty()) {
    g_skin.loaded = g_skin.record;
    return;
  }
  __imp__sub_822ED738(ctx, base);
}

// bonusMenuActiveBonus(item) -- on the joiner's console Kameo's costume line steps the joiner's own skin and tells the
// host; the host's steps the shared one as before.
extern "C" REX_FUNC(sub_824497F8) {
  if (ctx.r3.u32 != kBonusKameoSkinItem || !NetGame(base) || IsHost(base)) {
    __imp__sub_824497F8(ctx, base);
    return;
  }
  Choose(StepSuffix(ctx, base, kameo::engine::CostumeStepBack()));
  SendChoice(ctx, base);
}

// The pause menu's Costume line. Its steps run without the guest's context, so the skins' order and names are read on
// the game tick, and a choice is sent from there.
namespace {
std::vector<std::string> g_order{""};
std::unordered_map<std::string, std::u16string> g_names;
bool g_sendPending = false;
uint32_t g_ticksSinceSend = 0;
}  // namespace

bool kameo::engine::JoinerSkinOptionShown() {
  uint8_t* base = GuestBase();
  return base && NetGame(base) && !IsHost(base);
}

std::u16string kameo::engine::JoinerSkinName() {
  const std::string& suffix = g_skin.suffix;
  if (suffix.empty()) return kameo::l10n::Tr(u"Standard");
  const auto found = g_names.find(suffix);
  return found != g_names.end() ? found->second : std::u16string(suffix.begin(), suffix.end());
}

void kameo::engine::StepJoinerSkin(int direction) {
  const std::string current = g_skin.suffix;
  size_t at = 0;
  for (size_t i = 0; i < g_order.size(); ++i) {
    if (g_order[i] == current) at = i;
  }
  at = direction < 0 ? (at + g_order.size() - 1) % g_order.size() : (at + 1) % g_order.size();
  Choose(g_order[at]);
  g_sendPending = true;
}

void kameo::engine::TickJoinerSkin(PPCContext& ctx, uint8_t* base) {
  if (!NetGame(base) || IsHost(base)) return;
  const uint32_t node = KameoNode(base);
  static uint32_t tick = 0;
  if (tick++ % 60 == 0) {
    std::vector<std::string> order{""};
    for (uint32_t entry = node ? REX_LOAD_U32(node + 32 + 4) : 0; entry; entry = REX_LOAD_U32(entry + 4)) {
      if (!Call(ctx, base, sub_8225E158, entry)) continue;  // objectAlternativeModelUnlockedCheck
      const std::string name = GuestText(base, entry + 51);
      if (name.rfind("007183BF_", 0) != 0) continue;
      order.push_back(name.substr(9));
      if (!g_names.count(order.back())) g_names[order.back()] = KameoSkinDisplayName(ctx, base, name);
    }
    g_order = std::move(order);
  }
  // A choice goes to the host when made, and again now and then in case the host was loading a level then; the
  // standard look until the joiner picks one.
  if (g_sendPending || ++g_ticksSinceSend >= 600) {
    SendChoice(ctx, base);
    g_sendPending = false;
    g_ticksSinceSend = 0;
  }
}

#else

bool kameo::engine::HandleJoinerSkinMessage(uint8_t*, uint32_t) { return false; }
bool kameo::engine::JoinerSkinOptionShown() { return false; }
std::u16string kameo::engine::JoinerSkinName() { return {}; }
void kameo::engine::StepJoinerSkin(int) {}
void kameo::engine::TickJoinerSkin(PPCContext&, uint8_t*) {}

#endif  // KAMEO_TU
