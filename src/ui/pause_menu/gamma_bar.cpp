// The display Gamma bar drawn beside the game's own brightness bar on its brightness page.

#include <algorithm>
#include <cmath>
#include <string>

#include <rex/system/kernel_state.h>

#include "ui/pause_menu/pause_menu_internal.h"

using namespace kameo::ui;

namespace {

// The gamma bar is its own model instance of the brightness bar's asset, so the game's deferred drawing keeps the two segment counts apart.
struct GammaBar {
  uint32_t pause = 0;
  uint32_t model = 0;
  uint32_t fake_pause = 0;
  uint32_t fake_header = 0;
  uint32_t part_names[29] = {};
  bool ready = false;
};
GammaBar g_gammaBar;

constexpr uint32_t kGammaBarBufferTag = 144595370;
constexpr uint32_t kObjectHeaderBytes = 0x100;

void ShowBarSegments(PPCContext& ctx, uint8_t* base, uint32_t model, int32_t lit) {
  for (int32_t i = 1; i <= kGammaBarSegments; ++i) {
    PPCContext part = ctx;
    part.r3.u64 = model;
    part.r4.u64 = g_gammaBar.part_names[i];
    part.r5.u64 = i <= lit ? 1 : 0;
    sub_8224CFA0(part, base);
  }
}

// The bar's matrix slot that processGammaBar fills with the y of 220.
uint32_t BarYSlot(uint8_t* base, uint32_t matrix) {
  for (uint32_t slot : {matrix + 52, matrix + 28}) {
    if (std::fabs(LoadFloat(base, slot) - 220.0f) < 1.0f) return slot;
  }
  return 0;
}

}  // namespace

// storybookSimplePauseObj_renderbuffer(pauseObj) -- on the brightness page, moves the game's bar to its line and hands the render thread a second bar for gamma.
extern "C" REX_FUNC(sub_823EFAF8) {
  const uint32_t pause = ctx.r3.u32;
  const uint32_t record = pause ? REX_LOAD_U32(pause + 512) : 0;
  const uint32_t window = record ? REX_LOAD_U32(record + 68) : 0;
  const uint32_t game_model = pause ? REX_LOAD_U32(pause + 264) : 0;
  g_gammaBar.ready = false;
  const uint32_t y_slot = record ? BarYSlot(base, record) : 0;
  if (!window || !game_model || !y_slot || REX_LOAD_U32(record + 120) != 2) {
    __imp__sub_823EFAF8(ctx, base);
    return;
  }
  const uint32_t text_object = REX_LOAD_U32(window + kWindowTextObject);
  // The bar model is about 38 units tall and drawn in an ortho view whose y runs up from the bottom of the 480-unit screen, so each bar's centre goes just under its label, flipped.
  constexpr float kBarCentreBelowLabel = 31.0f;
  constexpr float kOrthoHeight = 480.0f;
  const float brightness_label = TextLineCentre(base, text_object, kItemBrightnessLabel);
  const float gamma_label = TextLineCentre(base, text_object, kItemGammaLabel);
  const float brightness_y = kOrthoHeight - (brightness_label + kBarCentreBelowLabel);
  const float gamma_y = kOrthoHeight - (gamma_label + kBarCentreBelowLabel);
  if (brightness_label < 0.0f || gamma_label < 0.0f) {
    __imp__sub_823EFAF8(ctx, base);
    return;
  }
  auto* memory = rex::system::kernel_state()->memory();
  const uint32_t header = REX_LOAD_U32(pause + 8);
  if (g_gammaBar.pause != pause || !g_gammaBar.model) {
    // modelInitByAsset(header, asset) with the brightness bar's own asset.
    PPCContext create = ctx;
    create.r3.u64 = header;
    create.r4.u64 = REX_LOAD_U32(game_model + 64);
    sub_8224B618(create, base);
    g_gammaBar.model = create.r3.u32;
    g_gammaBar.pause = pause;
  }
  if (!g_gammaBar.fake_pause) {
    g_gammaBar.fake_pause = memory->SystemHeapAlloc(0x220);
    g_gammaBar.fake_header = memory->SystemHeapAlloc(kObjectHeaderBytes);
    for (int32_t i = 1; i <= kGammaBarSegments; ++i) {
      const std::string name = "|_PART_" + std::to_string(i);
      g_gammaBar.part_names[i] = memory->SystemHeapAlloc(uint32_t(name.size() + 1));
      for (size_t c = 0; g_gammaBar.part_names[i] && c <= name.size(); ++c) REX_STORE_U8(g_gammaBar.part_names[i] + uint32_t(c), c < name.size() ? uint8_t(name[c]) : 0);
    }
  }
  if (!g_gammaBar.model || !g_gammaBar.fake_pause || !g_gammaBar.fake_header || !g_gammaBar.part_names[kGammaBarSegments]) {
    __imp__sub_823EFAF8(ctx, base);
    return;
  }
  const float original_y = LoadFloat(base, y_slot);
  const uint32_t object_id = REX_LOAD_U32(header + 76);
  // A key no real object uses, so the gamma bar's buffers never collide with the pause object's.
  const uint32_t gamma_key = (object_id & 0xFFFF) | 0x7A5E0000;

  StoreFloat(base, y_slot, brightness_y);
  __imp__sub_823EFAF8(ctx, base);

  StoreFloat(base, y_slot, gamma_y);
  ShowBarSegments(ctx, base, g_gammaBar.model, GammaBarStep());
  // processGammaBar grows the bar whose line is selected and shrinks it back otherwise; the gamma bar keeps its own copy of that scale.
  static float gamma_scale = 0.0f;
  const float rest_scale = LoadFloat(base, 0x8273BB14);
  const float grow = LoadFloat(base, 0x8275A07C) * LoadFloat(base, 0x8273BB78);
  if (gamma_scale == 0.0f) gamma_scale = rest_scale;
  const bool gamma_selected = SelectedItemId(base, window, SelectedIndex(base, window)) == kItemGammaBar;
  gamma_scale = gamma_selected ? std::min(gamma_scale + grow, LoadFloat(base, 0x82711B10)) : std::max(gamma_scale - grow, rest_scale);
  PPCContext alloc = ctx;
  alloc.r3.u64 = 0;
  alloc.r4.u64 = 128;
  alloc.r5.u64 = kGammaBarBufferTag;
  alloc.r6.u64 = gamma_key;
  sub_822560C0(alloc, base);
  if (const uint32_t buffer = alloc.r3.u32) {
    const float game_scale = LoadFloat(base, record + 84);
    for (uint32_t offset = 0; offset < 128; offset += 4) REX_STORE_U32(buffer + offset, REX_LOAD_U32(record + offset));
    // The matrix has no rotation, so its scale sits on the diagonal; +84 is the scale renderGammaBar offsets the drop shadow by.
    if (game_scale > 0.0f) {
      StoreFloat(base, buffer, LoadFloat(base, buffer) * gamma_scale / game_scale);
      StoreFloat(base, buffer + 20, LoadFloat(base, buffer + 20) * gamma_scale / game_scale);
    }
    StoreFloat(base, buffer + 84, gamma_scale);
  }
  StoreFloat(base, y_slot, original_y);
  PPCContext buffer_model = ctx;
  buffer_model.r3.u64 = g_gammaBar.model;
  buffer_model.r4.u64 = gamma_key;
  sub_8224BE98(buffer_model, base);

  // renderGammaBar reads the model and matrix through the object's header id, so the second draw goes through a stand-in object carrying the gamma key.
  for (uint32_t offset = 0; offset < kObjectHeaderBytes; offset += 4) REX_STORE_U32(g_gammaBar.fake_header + offset, REX_LOAD_U32(header + offset));
  REX_STORE_U32(g_gammaBar.fake_header + 76, gamma_key);
  REX_STORE_U32(g_gammaBar.fake_pause + 8, g_gammaBar.fake_header);
  REX_STORE_U32(g_gammaBar.fake_pause + 512, record);
  g_gammaBar.ready = true;
}

// renderGammaBar(pauseObj) -- the game's brightness bar, then the gamma bar through its stand-in object.
extern "C" REX_FUNC(sub_823F1B88) {
  const bool draw_gamma = g_gammaBar.ready && ctx.r3.u32 == g_gammaBar.pause;
  __imp__sub_823F1B88(ctx, base);
  if (!draw_gamma) return;
  PPCContext gamma = ctx;
  gamma.r3.u64 = g_gammaBar.fake_pause;
  __imp__sub_823F1B88(gamma, base);
}

// storybookSimplePauseGammaMenuPressBCallback(window, _, pauseObj) -- returns to Video when the brightness page was opened from there.
extern "C" REX_FUNC(sub_823F0E98) {
  if (!g_brightnessFromDisplay) {
    __imp__sub_823F0E98(ctx, base);
    return;
  }
  g_brightnessFromDisplay = false;
  const uint32_t window = ctx.r3.u32;
  const uint32_t pause = ctx.r5.u32;
  // The same reset of the bar's animation the game's handler performs before leaving.
  REX_STORE_U32(REX_LOAD_U32(pause + 512) + 84, REX_LOAD_U32(0x8273BB14));
  ActivatePage(ctx, base, pause, window, Page::Video);
  KeepCursor(ctx, base, window, g_brightnessIndex);
}
