// Left and right on the Bonus pages' Kameo costume line step back and forward through her costumes; A only steps forward.

#include "engine/dlc/dlc.h"
#include "input/guest_controls.h"
#include "ui/text_window.h"

using namespace kameo::ui;
using namespace kameo::input;

namespace {

// The storybook status object keeps its state at +512: +40 the overlay open, +76 its text window, +116 set while a
// costume change waits for its model to load before the overlay is rebuilt.
constexpr uint32_t kStatusState = 512;
constexpr uint32_t kStateOverlay = 40;
constexpr uint32_t kStateWindow = 76;
constexpr uint32_t kStateCostumeLoading = 116;
constexpr uint32_t kBonusOverlay = 4810179;
constexpr uint32_t kBonusKameoSkinItem = 159937678;
// bonusOptionChanged, set by the line's own A press.
constexpr uint32_t kBonusOptionChanged = 0x82B72294;
// The sound the pages play when turned.
constexpr uint32_t kPageSound = 0xD0B7CAE;
// D-pad left and right; the game's own trigger bits beside them turn the pages, so they are left alone.
constexpr uint32_t kDpadLeft = 0x4;
constexpr uint32_t kDpadRight = 0x8;
constexpr float kStickPush = 0.5f;

}  // namespace

// _storybookGameStatusObj_tick(statusObj) -- the storybook's status pages.
extern "C" REX_FUNC(sub_823C62F8) {
  static int held_direction = 0;
  const uint32_t object = ctx.r3.u32;
  const uint32_t state = object ? REX_LOAD_U32(object + kStatusState) : 0;
  const uint32_t window = state && REX_LOAD_U32(state + kStateOverlay) == kBonusOverlay ? REX_LOAD_U32(state + kStateWindow) : 0;
  int direction = 0;
  if (window && SelectedItemId(base, window, SelectedIndex(base, window)) == kBonusKameoSkinItem) {
    const uint32_t stick = PlayerStick(base, int8_t(REX_LOAD_U8(REX_LOAD_U32(object + 8) + 146)));
    const uint32_t held = REX_LOAD_U32(stick + kStickHeld) & REX_LOAD_U32(kCommitTestButtonMask);
    const float x = LoadFloat(base, stick + kStickLeftX);
    if (x > kStickPush || (held & kDpadRight) != 0) direction = 1;
    if (x < -kStickPush || (held & kDpadLeft) != 0) direction = -1;
  }
  // One step per push: a costume takes a moment to load, and the game ignores a step while one is loading.
  const bool step = direction != 0 && direction != held_direction && !REX_LOAD_U32(state + kStateCostumeLoading);
  held_direction = direction;
  if (step) {
    REX_STORE_U32(kBonusOptionChanged, 1);
    kameo::engine::SetKameoCostumeStepBack(direction < 0);
    // bonusMenuActiveBonus(item), as the line's A press calls it.
    CallGuest(ctx, base, sub_823C1F80, kBonusKameoSkinItem);
    kameo::engine::SetKameoCostumeStepBack(false);
    REX_STORE_U32(state + kStateCostumeLoading, 1);
    CallGuest(ctx, base, sub_822EA728, kPageSound);
  }
  __imp__sub_823C62F8(ctx, base);
}
