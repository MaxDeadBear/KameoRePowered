// Left and right on a skin line step back and forward through the skins, where A only steps forward: Kameo's costume
// line on the Bonus pages, and Change Skin on a warrior's page.

#include <rex/logging.h>

#include "engine/dlc/dlc.h"
#include "input/guest_controls.h"
#include "ui/hud/fps_counter.h"
#include "ui/text_window.h"

using namespace kameo::ui;
using namespace kameo::input;

namespace {

// Both storybook objects keep their state at +512.
constexpr uint32_t kObjectState = 512;
// The status pages' state: +40 the overlay open, +76 its text window, +116 set while a costume change waits for its
// model to load before the overlay is rebuilt.
constexpr uint32_t kStatusOverlay = 40;
constexpr uint32_t kStatusWindow = 76;
constexpr uint32_t kStatusCostumeLoading = 116;
constexpr uint32_t kBonusOverlay = 4810179;
constexpr uint32_t kBonusKameoSkinItem = 159937678;
// bonusOptionChanged, set by the line's own A press.
constexpr uint32_t kBonusOptionChanged = 0x82B72294;
// A warrior page's state: +28 the warrior shown, whose model type is at *(*(+264)+64)+4, and +36 its text window.
constexpr uint32_t kWarriorShown = 28;
constexpr uint32_t kWarriorWindow = 36;
constexpr uint32_t kWarriorSkinItem = 9464030;
// The tutorial event the skin lines check first, as their A press does.
constexpr uint32_t kSkinTutorial = 0x9068DE;
// The sound the pages play when turned.
constexpr uint32_t kPageSound = 0xD0B7CAE;
// D-pad left and right; the game's own trigger bits beside them turn the pages, so they are left alone.
constexpr uint32_t kDpadLeft = 0x4;
constexpr uint32_t kDpadRight = 0x8;
constexpr float kStickPush = 0.5f;

// -1 or 1 on the first frame of a push left or right while `item` is the line selected in `window`, else 0. One step
// per push: a skin takes a moment to load, and the game ignores a step while one is loading.
int Push(uint8_t* base, uint32_t object, uint32_t window, uint32_t item, int& held_direction) {
  int direction = 0;
  if (window && SelectedItemId(base, window, SelectedIndex(base, window)) == item) {
    const uint32_t stick = PlayerStick(base, int8_t(REX_LOAD_U8(REX_LOAD_U32(object + 8) + 146)));
    const uint32_t held = REX_LOAD_U32(stick + kStickHeld) & REX_LOAD_U32(kCommitTestButtonMask);
    const float x = LoadFloat(base, stick + kStickLeftX);
    if (x > kStickPush || (held & kDpadRight) != 0) direction = 1;
    if (x < -kStickPush || (held & kDpadLeft) != 0) direction = -1;
  }
  const int step = direction != held_direction ? direction : 0;
  held_direction = direction;
  return step;
}

}  // namespace

// _storybookGameStatusObj_tick(statusObj) -- the storybook's status pages.
extern "C" REX_FUNC(sub_823C62F8) {
  static int held_direction = 0;
  const uint32_t object = ctx.r3.u32;
  const uint32_t state = object ? REX_LOAD_U32(object + kObjectState) : 0;
  const uint32_t window = state && REX_LOAD_U32(state + kStatusOverlay) == kBonusOverlay ? REX_LOAD_U32(state + kStatusWindow) : 0;
  if (const int step = Push(base, object, window, kBonusKameoSkinItem, held_direction); step && !REX_LOAD_U32(state + kStatusCostumeLoading)) {
    REX_STORE_U32(kBonusOptionChanged, 1);
    kameo::engine::SetCostumeStepBack(step < 0);
    // bonusMenuActiveBonus(item), as the line's A press calls it.
    CallGuest(ctx, base, sub_823C1F80, kBonusKameoSkinItem);
    kameo::engine::SetCostumeStepBack(false);
    REX_STORE_U32(state + kStatusCostumeLoading, 1);
    CallGuest(ctx, base, sub_822EA728, kPageSound);
  }
  __imp__sub_823C62F8(ctx, base);
  // The Bonus page ignores the costume line while this is set; it clears once the skin record stops loading.
  static uint32_t last_loading = 0;
  const uint32_t loading = state && KameoGuestRangeReadable(state + kStatusCostumeLoading, 4) ? REX_LOAD_U32(state + kStatusCostumeLoading) : 0;
  if (loading != last_loading) {
    last_loading = loading;
    REXLOG_INFO("[kameo] bonus page costume {}: {}", loading ? "loading" : "loaded", kameo::engine::KameoSkinState(base));
  }
}

// _storybookMonsterInfoObj_tick(infoObj) -- a warrior's page.
extern "C" REX_FUNC(sub_823DA798) {
  static int held_direction = 0;
  const uint32_t object = ctx.r3.u32;
  const uint32_t state = object ? REX_LOAD_U32(object + kObjectState) : 0;
  const uint32_t window = state ? REX_LOAD_U32(state + kWarriorWindow) : 0;
  const uint32_t shown = state ? REX_LOAD_U32(state + kWarriorShown) : 0;
  const uint32_t model = shown ? REX_LOAD_U32(shown + 264) : 0;
  const uint32_t model_type = model ? REX_LOAD_U32(REX_LOAD_U32(model + 64) + 4) : 0;
  if (const int step = Push(base, object, window, kWarriorSkinItem, held_direction); step && model_type) {
    // As the line's A press: storybookTutorialEvent, then objectAlternativeModelIsLoading, then the next-skin call.
    PPCContext call = ctx;
    call.r3.u64 = kSkinTutorial;
    sub_822695C0(call, base);
    PPCContext loading = ctx;
    loading.r3.u64 = model_type;
    if (call.r3.u32) sub_8224FDD0(loading, base);
    if (call.r3.u32 && !loading.r3.u32) {
      kameo::engine::SetCostumeStepBack(step < 0);
      CallGuest(ctx, base, sub_8224F530, model_type);
      kameo::engine::SetCostumeStepBack(false);
      CallGuest(ctx, base, sub_822EA728, kPageSound);
    }
  }
  __imp__sub_823DA798(ctx, base);
}
