// Restore Defaults: which settings each page puts back, and the question asked first.

#include <string>
#include <vector>

#include "core/settings.h"
#include "gpu/textures/texture_pack.h"
#include "ui/pause_menu/pause_menu_internal.h"

namespace kameo::ui {

namespace {

// The settings each page's Restore Defaults puts back; a category's reset also covers the categories under it.
struct DefaultSetting {
  Page page;
  const char* cvar;
};

const DefaultSetting kDefaultSettings[] = {
    {Page::Video, "kameo_window_mode"}, {Page::Video, "kameo_gfx_widescreen"}, {Page::Video, "kameo_gfx_render_height"}, {Page::Video, "kameo_gfx_render_width"}, {Page::Video, "kameo_gfx_user_gamma"}, {Page::Hud, "kameo_hud_layout"}, {Page::Hud, "kameo_hud_scale"},
    {Page::Graphics, "kameo_gfx_backend"}, {Page::Graphics, "kameo_gfx_msaa"}, {Page::Graphics, "kameo_gfx_anisotropy"}, {Page::Graphics, "kameo_gfx_shadows"}, {Page::Graphics, "kameo_gfx_shadow_filtering"}, {Page::Graphics, "kameo_gfx_shadow_resolution"},
    {Page::Effects, "kameo_gfx_particles"}, {Page::Effects, "kameo_gfx_grass"}, {Page::Effects, "kameo_gfx_fur"}, {Page::Effects, "kameo_gfx_dust_motes"},{Page::Effects, "kameo_gfx_bloom"}, {Page::Effects, "kameo_gfx_depth_of_field"}, {Page::Effects, "kameo_gfx_heat_haze"},
    {Page::Video, "kameo_gfx_vsync"}, {Page::Video, "kameo_fps_counter"}, {Page::Video, "kameo_gfx_fps_cap"},
    {Page::Camera, "kameo_camera_drift"}, {Page::Camera, "kameo_camera_fov"}, {Page::Camera, "kameo_camera_auto_level"},
    {Page::Materials, "kameo_gfx_normal_maps"}, {Page::Materials, "kameo_gfx_bump_maps"}, {Page::Materials, "kameo_gfx_specular"},
    {Page::Audio, "kameo_volume_master"}, {Page::Audio, "kameo_volume_music"}, {Page::Audio, "kameo_volume_sfx"}, {Page::Audio, "kameo_volume_ambience"}, {Page::Audio, "kameo_volume_speech"}, {Page::Audio, "kameo_volume_cutscenes"},
    {Page::Gameplay, "kameo_language"},
    {Page::Mods, "kameo_gfx_texture_replacements"}, {Page::Mods, "kameo_gfx_dump_textures"}, {Page::Mods, "kameo_movie_replacements"},
    {Page::Coop, "kameo_story_splitscreen"}, {Page::Gameplay, "kameo_unlock_all_extras"}, {Page::Gameplay, "kameo_unlock_all_warriors"}, {Page::Gameplay, "kameo_skip_cutscenes"},
    {Page::Controls, "kameo_input_mode"}, {Page::Coop, "kameo_p2_input_mode"}, {Page::Controls, "kameo_p1_keyboard_device"}, {Page::Coop, "kameo_p2_keyboard_device"}, {Page::Controls, "kameo_p1_mouse_device"}, {Page::Coop, "kameo_p2_mouse_device"}, {Page::Controls, "mnk_mouse"}, {Page::Controls, "mnk_sensitivity"},
    {Page::PadBindings, "padbind_a"}, {Page::PadBindings, "padbind_b"}, {Page::PadBindings, "padbind_x"}, {Page::PadBindings, "padbind_y"},
    {Page::PadBindings, "padbind_left_shoulder"}, {Page::PadBindings, "padbind_right_shoulder"}, {Page::PadBindings, "padbind_left_trigger"}, {Page::PadBindings, "padbind_right_trigger"},
    {Page::PadBindings, "padbind_left_stick_press"}, {Page::PadBindings, "padbind_right_stick_press"},
    {Page::PadBindings, "padbind_dpad_up"}, {Page::PadBindings, "padbind_dpad_down"}, {Page::PadBindings, "padbind_dpad_left"}, {Page::PadBindings, "padbind_dpad_right"},
    {Page::PadBindings, "padbind_start"}, {Page::PadBindings, "padbind_back"},
    {Page::PadBindings, "padbind_left_up"}, {Page::PadBindings, "padbind_left_down"}, {Page::PadBindings, "padbind_left_left"}, {Page::PadBindings, "padbind_left_right"},
    {Page::PadBindings, "padbind_right_up"}, {Page::PadBindings, "padbind_right_down"}, {Page::PadBindings, "padbind_right_left"}, {Page::PadBindings, "padbind_right_right"},
    {Page::Bindings, "keybind_a"}, {Page::Bindings, "keybind_b"}, {Page::Bindings, "keybind_x"}, {Page::Bindings, "keybind_y"},
    {Page::Bindings, "keybind_left_trigger"}, {Page::Bindings, "keybind_right_trigger"}, {Page::Bindings, "keybind_left_shoulder"}, {Page::Bindings, "keybind_right_shoulder"},
    {Page::Bindings, "keybind_lstick_up"}, {Page::Bindings, "keybind_lstick_down"}, {Page::Bindings, "keybind_lstick_left"}, {Page::Bindings, "keybind_lstick_right"}, {Page::Bindings, "keybind_lstick_press"},
    {Page::Bindings, "keybind_rstick_up"}, {Page::Bindings, "keybind_rstick_down"}, {Page::Bindings, "keybind_rstick_left"}, {Page::Bindings, "keybind_rstick_right"}, {Page::Bindings, "keybind_rstick_press"},
    {Page::Bindings, "keybind_dpad_up"}, {Page::Bindings, "keybind_dpad_down"}, {Page::Bindings, "keybind_dpad_left"}, {Page::Bindings, "keybind_dpad_right"},
    {Page::Bindings, "keybind_back"}, {Page::Bindings, "keybind_start"},
};

// Which settings a pending Restore Defaults covers (None for all of them), the page to go back to, and the line the cursor was on there.
Page g_resetScope = Page::None;
Page g_resetReturn = Page::None;
uint32_t g_resetReturnIndex = 0;

bool PageWithin(Page page, Page scope) {
  for (const PageInfo* info = FindPage(page); info; info = FindPage(info->parent)) {
    if (info->page == scope) return true;
  }
  return false;
}

void RestoreDefaults(Page scope) {
  std::vector<std::string> names;
  bool textures = false;
  for (const DefaultSetting& setting : kDefaultSettings) {
    if (scope != Page::None && !PageWithin(setting.page, scope)) continue;
    names.push_back(setting.cvar);
    textures = textures || setting.page == Page::Mods;
  }
  KameoRestoreDefaultSettings(names);
  if (textures) kameo::gpu::RefreshAllTextureReplacements();
}

std::u16string ResetQuestion(Page scope) {
  const PageInfo* info = FindPage(scope);
  if (!info) return u"Restore all settings to their defaults?";
  return u"Restore " + std::u16string(info->title) + u" to defaults?";
}

}  // namespace

bool HasDefaults(Page scope) {
  for (const DefaultSetting& setting : kDefaultSettings) {
    if (PageWithin(setting.page, scope)) return true;
  }
  return false;
}

void AddConfirmResetRows(PPCContext& ctx, uint8_t* base, uint32_t window) {
  AddPageTitle(ctx, base, window, ResetQuestion(g_resetScope));
  AddCategoryLine(ctx, base, window, kItemConfirmRestore, u"Restore Defaults");
  AddCategoryLine(ctx, base, window, kItemCancelRestore, u"Cancel");
}

bool PressRestoreDefaultsItem(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window, uint32_t id, uint32_t index) {
  if (id == kItemRestoreDefaults || id == kItemRestoreAllDefaults) {
    g_resetScope = id == kItemRestoreAllDefaults ? Page::None : g_currentPage;
    g_resetReturn = g_currentPage;
    g_resetReturnIndex = index;
    ActivatePage(ctx, base, pause, window, Page::ConfirmReset);
    // The cursor starts on Cancel, so a stray second press does not wipe anything.
    KeepCursor(ctx, base, window, 1);
    return true;
  }
  if (id == kItemConfirmRestore || id == kItemCancelRestore) {
    if (id == kItemConfirmRestore) RestoreDefaults(g_resetScope);
    ReturnFromReset(ctx, base, pause, window);
    return true;
  }
  return false;
}

void ReturnFromReset(PPCContext& ctx, uint8_t* base, uint32_t pause, uint32_t window) {
  const Page page = g_resetReturn;
  if (page == Page::None) {
    g_currentPage = Page::None;
    PPCContext back = ctx;
    back.r3.u64 = window;
    back.r5.u64 = pause;
    __imp__sub_823F1028(back, base);
  } else {
    ActivatePage(ctx, base, pause, window, page);
  }
  KeepCursor(ctx, base, window, g_resetReturnIndex);
}

}  // namespace kameo::ui
