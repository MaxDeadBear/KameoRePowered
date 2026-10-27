// Every option the pause menu's pages show, in page order.

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>

#include "app/window_mode.h"
#include "core/settings.h"
#include "core/unlock_codes.h"
#include "engine/audio.h"
#include "gpu/config.h"
#include "gpu/movie/movie_replace.h"
#include "gpu/stereo/stereo.h"  // STEREO:
#include "gpu/textures/texture_pack.h"
#include "input/guest_controls.h"
#include "input/input.h"
#include "ui/pause_menu/option_values.h"
#include "ui/pause_menu/pause_menu_internal.h"

REXCVAR_DECLARE(bool, kameo_story_splitscreen);
REXCVAR_DECLARE(bool, kameo_unlock_all_extras);
REXCVAR_DECLARE(bool, kameo_unlock_all_warriors);
REXCVAR_DECLARE(bool, kameo_skip_cutscenes);
REXCVAR_DECLARE(int32_t, kameo_language);
REXCVAR_DECLARE(int32_t, kameo_input_mode);
REXCVAR_DECLARE(int32_t, kameo_p1_keyboard_device);
REXCVAR_DECLARE(int32_t, kameo_p2_keyboard_device);
REXCVAR_DECLARE(int32_t, kameo_p1_mouse_device);
REXCVAR_DECLARE(int32_t, kameo_p2_mouse_device);
REXCVAR_DECLARE(bool, mnk_mouse);
REXCVAR_DECLARE(double, mnk_sensitivity);
REXCVAR_DECLARE(bool, kameo_camera_drift);
REXCVAR_DECLARE(int32_t, kameo_camera_fov);
REXCVAR_DECLARE(bool, kameo_camera_auto_level);
REXCVAR_DECLARE(bool, kameo_fps_counter);

namespace kameo::ui {

namespace {

using kameo::input::kPlayerControlSettings;

#define KAMEO_GAME_OPTION(id, page, label, game_value, step, shown) {id, page, label, nullptr, step, game_value, shown}

#define KAMEO_BOOL_OPTION(id, page, label, name) {id, page, label, [] { return OnOff(REXCVAR_GET(name)); }, [](int, bool) { REXCVAR_SET(name, !REXCVAR_GET(name)); }}

// A setting that only shows once its cheat code has been entered.
#define KAMEO_HIDDEN_BOOL_OPTION(id, page, label, name, code) {id, page, label, [] { return OnOff(REXCVAR_GET(name)); }, [](int, bool) { REXCVAR_SET(name, !REXCVAR_GET(name)); }, nullptr, [] { return KameoCodeUnlocked(code); }}

#define KAMEO_BIND_OPTION(id, label, cvar) {id, Page::Bindings, label, [] { return BindingValue(cvar); }, [](int, bool) { KameoStartKeyCapture(cvar); }}
#define KAMEO_PAD_OPTION(id, label, cvar) {id, Page::PadBindings, label, [] { return KameoPadBindingLabel(cvar); }, [](int, bool) { KameoStartPadCapture(cvar); }}

#define KAMEO_VOLUME_OPTION(id, label) {id, Page::Audio, label, [] { return Number(VolumeFor(id)->load(), u"%"); }, [](int direction, bool wrap) { VolumeFor(id)->store(Step(VolumeFor(id)->load(), {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}, direction, wrap)); g_kameo_volume_dirty.store(1); }}

const Option kOptions[] = {
    {0x7A5E000B, Page::Video, u"Display Mode", [] { const int32_t mode = KameoWindowMode(); return std::u16string(mode == 0 ? u"Windowed" : mode == 1 ? u"Borderless" : u"Fullscreen"); }, [](int direction, bool wrap) { KameoSetWindowMode(Step(KameoWindowMode(), {0, 1, 2}, direction, wrap)); }},
    {0x7A5E0002, Page::Video, u"Resolution", [] { return ResolutionText(); }, [](int direction, bool wrap) { StepResolution(direction, wrap); }},
    KAMEO_BOOL_OPTION(0x7A5E0001, Page::Video, u"Widescreen", kameo_gfx_widescreen),
    KAMEO_BOOL_OPTION(0x7A5E000F, Page::Video, u"VSync", kameo_gfx_vsync),
    {0x7A5E000E, Page::Video, u"FPS Limit", [] { const int32_t cap = REXCVAR_GET(kameo_gfx_fps_cap); return cap <= 0 ? std::u16string(u"Off") : Number(cap, u""); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_fps_cap, Step(REXCVAR_GET(kameo_gfx_fps_cap), {0, 30, 60, 120, 144, 165, 240}, direction, wrap)); }},
    KAMEO_BOOL_OPTION(0x7A5E000D, Page::Video, u"FPS Counter", kameo_fps_counter),
    {0x7A5E0024, Page::Hud, u"HUD Layout", [] { const int32_t layout = REXCVAR_GET(kameo_hud_layout); return std::u16string(layout == 0 ? u"Stretched" : layout == 1 ? u"Centred 16:9" : u"Screen Edges"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_hud_layout, Step(REXCVAR_GET(kameo_hud_layout), {0, 1, 2}, direction, wrap)); }},
    {0x7A5E0025, Page::Hud, u"HUD Scale", [] { return Number(REXCVAR_GET(kameo_hud_scale), u"%"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_hud_scale, Step(REXCVAR_GET(kameo_hud_scale), {50, 60, 70, 75, 80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130, 140, 150}, direction, wrap)); }},
    KAMEO_GAME_OPTION(0x7A5E0020, Page::Hud, u"Control Display", [] { const uint32_t prefs = GameSetting(kIconPrefs + 4 * g_menuPlayer); return ControlDisplayLocked() || prefs == 2 ? 661 : prefs == 1 ? 662 : 660; }, [](int direction, bool) { if (!ControlDisplayLocked()) SetGameSetting(kIconPrefs + 4 * g_menuPlayer, Cycle3(GameSetting(kIconPrefs + 4 * g_menuPlayer), direction)); }, nullptr),
    KAMEO_GAME_OPTION(0x7A5E0021, Page::Hud, u"Score Display", [] { const uint32_t score = GameSetting(kScoreDisplay + 4 * g_menuPlayer); return score == 1 ? 941 : score == 2 ? 940 : 939; }, [](int direction, bool) { SetGameSetting(kScoreDisplay + 4 * g_menuPlayer, Cycle3(GameSetting(kScoreDisplay + 4 * g_menuPlayer), direction)); }, nullptr),
    KAMEO_GAME_OPTION(0x7A5E0022, Page::Hud, u"Map Display", [] { return GameSetting(kMapDisplay + 4 * g_menuPlayer) == 1 ? 4464 : 4465; }, [](int, bool) { SetGameSetting(kMapDisplay + 4 * g_menuPlayer, GameSetting(kMapDisplay + 4 * g_menuPlayer) == 1 ? 2 : 1); }, [] { return GameSetting(kNumWantedPlayers) == 1; }),
    {0x7A5E002A, Page::Graphics, u"Renderer", [] { return std::u16string(ChosenVulkan() ? u"Vulkan" : u"Direct3D 12") + (ChosenVulkan() != RunningVulkan() ? u" (restart)" : u""); }, [](int, bool) { REXCVAR_SET(kameo_gfx_backend, std::string(ChosenVulkan() ? "d3d12" : "vulkan")); }},
    {0x7A5E0003, Page::Graphics, u"Anti-aliasing", [] { const int32_t samples = REXCVAR_GET(kameo_gfx_msaa); return samples <= 1 ? std::u16string(u"Off") : Number(samples, u"x"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_msaa, Step(REXCVAR_GET(kameo_gfx_msaa), {0, 2, 4, 8}, direction, wrap)); }},
    {0x7A5E0004, Page::Graphics, u"Anisotropic Filtering", [] { const int32_t level = REXCVAR_GET(kameo_gfx_anisotropy); return level <= 1 ? std::u16string(u"Game") : Number(level, u"x"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_anisotropy, Step(REXCVAR_GET(kameo_gfx_anisotropy), {0, 2, 4, 8, 16}, direction, wrap)); }},
    KAMEO_BOOL_OPTION(0x7A5E0005, Page::Graphics, u"Shadows", kameo_gfx_shadows),
    {0x7A5E000C, Page::Graphics, u"Shadow Quality", [] { const int32_t scale = REXCVAR_GET(kameo_gfx_shadow_resolution); return std::u16string(scale >= 4 ? u"Ultra" : scale >= 2 ? u"High" : u"Original"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_shadow_resolution, Step(REXCVAR_GET(kameo_gfx_shadow_resolution), {1, 2, 4}, direction, wrap)); }},
    {0x7A5E0010, Page::Graphics, u"Shadow Edges", [] { return std::u16string(REXCVAR_GET(kameo_gfx_shadow_filtering) ? u"Smooth" : u"Pixelated"); }, [](int, bool) { REXCVAR_SET(kameo_gfx_shadow_filtering, !REXCVAR_GET(kameo_gfx_shadow_filtering)); }},
    // STEREO: the four 3D rows.
    {0x7A5E002B, Page::Graphics, u"3D Display", [] { const int32_t mode = REXCVAR_GET(kameo_gfx_stereo); return std::u16string(mode == 1 ? u"Side by Side" : mode == 2 ? u"Top and Bottom" : mode == 3 ? u"Red/Cyan Glasses" : u"Off"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_stereo, Step(REXCVAR_GET(kameo_gfx_stereo), {0, 1, 2, 3}, direction, wrap)); }},
    {0x7A5E002C, Page::Graphics, u"3D Depth", [] { const int32_t depth = REXCVAR_GET(kameo_gfx_stereo_depth); return Number(depth / 10, u".") + Number(depth % 10, u"%"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_stereo_depth, Step(REXCVAR_GET(kameo_gfx_stereo_depth), {5, 10, 15, 20, 25, 30, 35, 40, 50, 60, 80, 100}, direction, wrap)); }, nullptr, [] { return REXCVAR_GET(kameo_gfx_stereo) != 0; }},
    {0x7A5E002D, Page::Graphics, u"3D Focus", [] { const int32_t focus = REXCVAR_GET(kameo_gfx_stereo_focus); return focus == 100 ? std::u16string(u"On Kameo") : Number(focus, u"%"); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_gfx_stereo_focus, Step(REXCVAR_GET(kameo_gfx_stereo_focus), {25, 50, 75, 100, 150, 200, 300, 500}, direction, wrap)); }, nullptr, [] { return REXCVAR_GET(kameo_gfx_stereo) != 0; }},
    {0x7A5E002E, Page::Graphics, u"3D Swap Eyes", [] { return OnOff(REXCVAR_GET(kameo_gfx_stereo_swap)); }, [](int, bool) { REXCVAR_SET(kameo_gfx_stereo_swap, !REXCVAR_GET(kameo_gfx_stereo_swap)); }, nullptr, [] { return REXCVAR_GET(kameo_gfx_stereo) != 0; }},
    KAMEO_BOOL_OPTION(0x7A5E0006, Page::Effects, u"Particles", kameo_gfx_particles),
    KAMEO_BOOL_OPTION(0x7A5E0007, Page::Effects, u"Grass", kameo_gfx_grass),
    KAMEO_BOOL_OPTION(0x7A5E0028, Page::Effects, u"Fur", kameo_gfx_fur),
    KAMEO_BOOL_OPTION(0x7A5E0029, Page::Effects, u"Dust Motes", kameo_gfx_dust_motes),
    KAMEO_BOOL_OPTION(0x7A5E0008, Page::Effects, u"Bloom", kameo_gfx_bloom),
    KAMEO_BOOL_OPTION(0x7A5E0009, Page::Effects, u"Depth of Field", kameo_gfx_depth_of_field),
    KAMEO_BOOL_OPTION(0x7A5E000A, Page::Effects, u"Heat Haze", kameo_gfx_heat_haze),
    KAMEO_BOOL_OPTION(0x7A5E0017, Page::Materials, u"Normal Maps", kameo_gfx_normal_maps),
    KAMEO_BOOL_OPTION(0x7A5E0018, Page::Materials, u"Bump Maps", kameo_gfx_bump_maps),
    KAMEO_BOOL_OPTION(0x7A5E0019, Page::Materials, u"Specular", kameo_gfx_specular),
    KAMEO_VOLUME_OPTION(0x7A5E0101, u"Master Volume"),
    KAMEO_VOLUME_OPTION(0x7A5E0102, u"Music"),
    KAMEO_VOLUME_OPTION(0x7A5E0103, u"Sound Effects"),
    KAMEO_VOLUME_OPTION(0x7A5E0104, u"Ambience"),
    KAMEO_VOLUME_OPTION(0x7A5E0105, u"Speech"),
    KAMEO_VOLUME_OPTION(0x7A5E0106, u"Cutscenes"),
    {0x7A5E0401, Page::Controls, u"Player 1 Input", [] { return std::u16string(REXCVAR_GET(kameo_input_mode) == 1 ? u"Gamepad" : u"Keyboard & Mouse"); }, [](int, bool) { KameoSetPlayerOneInputMode(REXCVAR_GET(kameo_input_mode) == 1 ? 0 : 1); }, nullptr, [] { return REXCVAR_GET(kameo_story_splitscreen); }},
    {0x7A5E0406, Page::Controls, u"Controller Preset", [] { const int preset = KameoPadPreset(); return std::u16string(preset == 0 ? u"Button Position" : preset == 1 ? u"Button Labels" : u"Custom"); }, [](int direction, bool wrap) { const int preset = KameoPadPreset(); KameoSetPadPreset(preset == 2 ? (direction < 0 ? 0 : 1) : Step(preset, {0, 1}, direction, wrap)); }},
    KAMEO_BOOL_OPTION(0x7A5E0402, Page::Controls, u"Mouse Look", mnk_mouse),
    {0x7A5E0403, Page::Controls, u"Mouse Sensitivity", [] { return Sensitivity(); }, [](int direction, bool wrap) { int32_t tenths = int32_t(std::lround(REXCVAR_GET(mnk_sensitivity) * 10.0)) + direction; if (tenths > 100) tenths = wrap ? 1 : 100; REXCVAR_SET(mnk_sensitivity, std::clamp(tenths, 1, 100) / 10.0); }},
    {0x7A5E0430, Page::Controls, u"Player 1 Keyboard", [] { return std::u16string(REXCVAR_GET(kameo_p1_keyboard_device) == 1 ? u"Keyboard 2" : u"Keyboard 1"); }, [](int, bool) { REXCVAR_SET(kameo_p1_keyboard_device, REXCVAR_GET(kameo_p1_keyboard_device) == 1 ? 0 : 1); }, nullptr, [] { return KameoPlayerUsesKeyboard(0); }},
    {0x7A5E0431, Page::Controls, u"Player 1 Mouse", [] { return std::u16string(REXCVAR_GET(kameo_p1_mouse_device) == 1 ? u"Mouse 2" : u"Mouse 1"); }, [](int, bool) { REXCVAR_SET(kameo_p1_mouse_device, REXCVAR_GET(kameo_p1_mouse_device) == 1 ? 0 : 1); }, nullptr, [] { return KameoPlayerUsesKeyboard(0); }},
    KAMEO_GAME_OPTION(0x7A5E0404, Page::Camera, u"Look Left/Right", [] { return GameSetting(kPlayerControlSettings + 148 * g_menuPlayer) != 0 ? 363 : 364; }, [](int, bool) { SetGameSetting(kPlayerControlSettings + 148 * g_menuPlayer, GameSetting(kPlayerControlSettings + 148 * g_menuPlayer) != 0 ? 0 : 1); }, nullptr),
    KAMEO_GAME_OPTION(0x7A5E0405, Page::Camera, u"Look Up/Down", [] { return GameSetting(kPlayerControlSettings + 4 + 148 * g_menuPlayer) != 0 ? 365 : 366; }, [](int, bool) { SetGameSetting(kPlayerControlSettings + 4 + 148 * g_menuPlayer, GameSetting(kPlayerControlSettings + 4 + 148 * g_menuPlayer) != 0 ? 0 : 1); }, nullptr),
    {0x7A5E001A, Page::Camera, u"Camera Follow", [] { return std::u16string(REXCVAR_GET(kameo_camera_drift) ? u"Original (drifts)" : u"Fixed Distance"); }, [](int, bool) { REXCVAR_SET(kameo_camera_drift, !REXCVAR_GET(kameo_camera_drift)); }},
    KAMEO_BOOL_OPTION(0x7A5E001C, Page::Camera, u"Auto-Level", kameo_camera_auto_level),
    {0x7A5E001B, Page::Camera, u"Field of View", [] { const int32_t fov = REXCVAR_GET(kameo_camera_fov); return fov == 0 ? std::u16string(u"Default") : Number(fov, u"").insert(0, fov > 0 ? u"+" : u""); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_camera_fov, Step(REXCVAR_GET(kameo_camera_fov), {-10, -5, 0, 5, 10, 15, 20, 25, 30}, direction, wrap)); }},
    {0x7A5E0203, Page::Coop, u"Story Split-screen", [] { return OnOff(REXCVAR_GET(kameo_story_splitscreen)); }, [](int, bool) { KameoSetCoopEnabled(!REXCVAR_GET(kameo_story_splitscreen)); }, nullptr, [] { return !KameoMenuCoop(); }},
    {0x7A5E0407, Page::Coop, u"Player 2 Input", [] { return std::u16string(KameoCoopInputMode() == 2 ? u"Keyboard & Mouse" : u"Gamepad"); }, [](int, bool) { KameoSetCoopInputMode(KameoCoopInputMode() == 2 ? 1 : 2); }, nullptr, [] { return REXCVAR_GET(kameo_story_splitscreen); }},
    KAMEO_GAME_OPTION(0x7A5E0023, Page::Coop, u"Split-Screen Layout", [] { return GameSetting(kSplitScreenMode) != 0 ? 4207 : 4208; }, [](int, bool) { SetGameSetting(kSplitScreenMode, (GameSetting(kSplitScreenMode) + 1) % 2); }, [] { return GameSetting(kIsSplitScreenMode) != 0; }),
    {0x7A5E0432, Page::Coop, u"Player 2 Keyboard", [] { return std::u16string(REXCVAR_GET(kameo_p2_keyboard_device) == 1 ? u"Keyboard 2" : u"Keyboard 1"); }, [](int, bool) { REXCVAR_SET(kameo_p2_keyboard_device, REXCVAR_GET(kameo_p2_keyboard_device) == 1 ? 0 : 1); }, nullptr, [] { return KameoPlayerUsesKeyboard(1); }},
    {0x7A5E0433, Page::Coop, u"Player 2 Mouse", [] { return std::u16string(REXCVAR_GET(kameo_p2_mouse_device) == 1 ? u"Mouse 2" : u"Mouse 1"); }, [](int, bool) { REXCVAR_SET(kameo_p2_mouse_device, REXCVAR_GET(kameo_p2_mouse_device) == 1 ? 0 : 1); }, nullptr, [] { return KameoPlayerUsesKeyboard(1); }},
    {0x7A5E0408, Page::Coop, u"Player 2 Aim X", [] { return std::u16string(GameSetting(kPlayerControlSettings + 148) != 0 ? u"Inverted" : u"Normal"); }, [](int, bool) { SetGameSetting(kPlayerControlSettings + 148, GameSetting(kPlayerControlSettings + 148) != 0 ? 0 : 1); }, nullptr, [] { return REXCVAR_GET(kameo_story_splitscreen); }},
    {0x7A5E0409, Page::Coop, u"Player 2 Aim Y", [] { return std::u16string(GameSetting(kPlayerControlSettings + 152) != 0 ? u"Inverted" : u"Normal"); }, [](int, bool) { SetGameSetting(kPlayerControlSettings + 152, GameSetting(kPlayerControlSettings + 152) != 0 ? 0 : 1); }, nullptr, [] { return REXCVAR_GET(kameo_story_splitscreen); }},
    KAMEO_GAME_OPTION(0x7A5E040A, Page::Coop, u"Player Pushing", [] { return GameSetting(kHostPush) != 0 ? 4125 : 4126; }, [](int, bool) { SetGameSetting(kHostPush, GameSetting(kHostPush) != 0 ? 0 : 1); }, [] { return GameSetting(kNumWantedPlayers) > 1 && g_menuPlayer == 0; }),
    KAMEO_GAME_OPTION(0x7A5E040B, Page::Coop, u"Friendly Fire", [] { return GameSetting(kHostAttack) != 0 ? 4127 : 4128; }, [](int, bool) { SetGameSetting(kHostAttack, GameSetting(kHostAttack) != 0 ? 0 : 1); }, [] { return GameSetting(kNumWantedPlayers) > 1 && g_menuPlayer == 0; }),
    KAMEO_BOOL_OPTION(0x7A5E0205, Page::Gameplay, u"Skippable Cutscenes", kameo_skip_cutscenes),
    KAMEO_GAME_OPTION(kItemHints, Page::Gameplay, u"Hints", [] { const uint32_t hints = GameSetting(kOrthoHints); return hints == 0 ? 4442 : hints == 1 ? 4443 : 4444; }, [](int direction, bool) { SetGameSetting(kOrthoHints, Cycle3(GameSetting(kOrthoHints), direction)); }, nullptr),
    {0x7A5E0107, Page::Gameplay, u"Language", [] { const int32_t language = ChosenLanguage(); return LanguageName(language) + (uint32_t(language) != KameoRunningLanguage() ? u" (restart)" : u""); }, [](int direction, bool wrap) { REXCVAR_SET(kameo_language, Step(ChosenLanguage(), {1, 2, 3, 4, 5, 6, 7, 8}, direction, wrap)); }},
    KAMEO_HIDDEN_BOOL_OPTION(0x7A5E0204, Page::Gameplay, u"Unlock All Extras", kameo_unlock_all_extras, KameoUnlockCode::GameCheats),
    KAMEO_HIDDEN_BOOL_OPTION(0x7A5E0207, Page::Gameplay, u"All Warriors & Upgrades", kameo_unlock_all_warriors, KameoUnlockCode::Warriors),
    {0x7A5E0201, Page::Mods, u"Texture Replacements", [] { return OnOff(REXCVAR_GET(kameo_gfx_texture_replacements)); }, [](int, bool) { REXCVAR_SET(kameo_gfx_texture_replacements, !REXCVAR_GET(kameo_gfx_texture_replacements)); kameo::gpu::RefreshAllTextureReplacements(); }},
    {0x7A5E0202, Page::Mods, u"Texture Dumping", [] { return OnOff(REXCVAR_GET(kameo_gfx_dump_textures)); }, [](int, bool) { REXCVAR_SET(kameo_gfx_dump_textures, !REXCVAR_GET(kameo_gfx_dump_textures)); if (REXCVAR_GET(kameo_gfx_dump_textures)) kameo::gpu::RequestDumpOfLoadedTextures(); }},
    KAMEO_BOOL_OPTION(0x7A5E0206, Page::Mods, u"Movie Replacements", kameo_movie_replacements),
    {kItemGammaBar, Page::GameBrightness, u"Gamma", [] { return std::u16string(); }, [](int direction, bool wrap) { int32_t step = GammaBarStep() + direction; if (wrap && step > kGammaBarSegments) step = 0; SetGammaBarStep(step); }},
    KAMEO_BIND_OPTION(0x7A5E0410, u"Move Forward", "keybind_lstick_up"),
    KAMEO_BIND_OPTION(0x7A5E0411, u"Move Back", "keybind_lstick_down"),
    KAMEO_BIND_OPTION(0x7A5E0412, u"Move Left", "keybind_lstick_left"),
    KAMEO_BIND_OPTION(0x7A5E0413, u"Move Right", "keybind_lstick_right"),
    KAMEO_BIND_OPTION(0x7A5E0414, u"\uE000 Button", "keybind_a"),
    KAMEO_BIND_OPTION(0x7A5E0415, u"\uE001 Button", "keybind_b"),
    KAMEO_BIND_OPTION(0x7A5E0416, u"\uE002 Button", "keybind_x"),
    KAMEO_BIND_OPTION(0x7A5E0417, u"\uE003 Button", "keybind_y"),
    KAMEO_BIND_OPTION(0x7A5E0418, u"Left Trigger", "keybind_left_trigger"),
    KAMEO_BIND_OPTION(0x7A5E0419, u"Right Trigger", "keybind_right_trigger"),
    KAMEO_BIND_OPTION(0x7A5E041A, u"Left Bumper", "keybind_left_shoulder"),
    KAMEO_BIND_OPTION(0x7A5E041B, u"Right Bumper", "keybind_right_shoulder"),
    KAMEO_BIND_OPTION(0x7A5E041C, u"Camera Up", "keybind_rstick_up"),
    KAMEO_BIND_OPTION(0x7A5E041D, u"Camera Down", "keybind_rstick_down"),
    KAMEO_BIND_OPTION(0x7A5E041E, u"Camera Left", "keybind_rstick_left"),
    KAMEO_BIND_OPTION(0x7A5E041F, u"Camera Right", "keybind_rstick_right"),
    KAMEO_BIND_OPTION(0x7A5E0420, u"Left Stick Click", "keybind_lstick_press"),
    KAMEO_BIND_OPTION(0x7A5E0421, u"Right Stick Click", "keybind_rstick_press"),
    KAMEO_BIND_OPTION(0x7A5E0422, u"D-Pad Up", "keybind_dpad_up"),
    KAMEO_BIND_OPTION(0x7A5E0423, u"D-Pad Down", "keybind_dpad_down"),
    KAMEO_BIND_OPTION(0x7A5E0424, u"D-Pad Left", "keybind_dpad_left"),
    KAMEO_BIND_OPTION(0x7A5E0425, u"D-Pad Right", "keybind_dpad_right"),
    KAMEO_BIND_OPTION(0x7A5E0426, u"Start", "keybind_start"),
    KAMEO_BIND_OPTION(0x7A5E0427, u"Back", "keybind_back"),
    KAMEO_PAD_OPTION(0x7A5E0501, u"\uE000 Action", "padbind_a"),
    KAMEO_PAD_OPTION(0x7A5E0502, u"\uE001 Action", "padbind_b"),
    KAMEO_PAD_OPTION(0x7A5E0503, u"\uE002 Action", "padbind_x"),
    KAMEO_PAD_OPTION(0x7A5E0504, u"\uE003 Action", "padbind_y"),
    KAMEO_PAD_OPTION(0x7A5E0505, u"Left Shoulder", "padbind_left_shoulder"),
    KAMEO_PAD_OPTION(0x7A5E0506, u"Right Shoulder", "padbind_right_shoulder"),
    KAMEO_PAD_OPTION(0x7A5E0507, u"Left Trigger", "padbind_left_trigger"),
    KAMEO_PAD_OPTION(0x7A5E0508, u"Right Trigger", "padbind_right_trigger"),
    KAMEO_PAD_OPTION(0x7A5E0509, u"Left Stick Press", "padbind_left_stick_press"),
    KAMEO_PAD_OPTION(0x7A5E050A, u"Right Stick Press", "padbind_right_stick_press"),
    KAMEO_PAD_OPTION(0x7A5E050B, u"D-Pad Up", "padbind_dpad_up"),
    KAMEO_PAD_OPTION(0x7A5E050C, u"D-Pad Down", "padbind_dpad_down"),
    KAMEO_PAD_OPTION(0x7A5E050D, u"D-Pad Left", "padbind_dpad_left"),
    KAMEO_PAD_OPTION(0x7A5E050E, u"D-Pad Right", "padbind_dpad_right"),
    KAMEO_PAD_OPTION(0x7A5E050F, u"Start", "padbind_start"),
    KAMEO_PAD_OPTION(0x7A5E0510, u"Back", "padbind_back"),
    KAMEO_PAD_OPTION(0x7A5E0511, u"Move Up", "padbind_left_up"),
    KAMEO_PAD_OPTION(0x7A5E0512, u"Move Down", "padbind_left_down"),
    KAMEO_PAD_OPTION(0x7A5E0513, u"Move Left", "padbind_left_left"),
    KAMEO_PAD_OPTION(0x7A5E0514, u"Move Right", "padbind_left_right"),
    KAMEO_PAD_OPTION(0x7A5E0515, u"Camera Up", "padbind_right_up"),
    KAMEO_PAD_OPTION(0x7A5E0516, u"Camera Down", "padbind_right_down"),
    KAMEO_PAD_OPTION(0x7A5E0517, u"Camera Left", "padbind_right_left"),
    KAMEO_PAD_OPTION(0x7A5E0518, u"Camera Right", "padbind_right_right"),
};

#undef KAMEO_BOOL_OPTION
#undef KAMEO_GAME_OPTION
#undef KAMEO_VOLUME_OPTION
#undef KAMEO_BIND_OPTION
#undef KAMEO_PAD_OPTION

}  // namespace

std::span<const Option> AllOptions() { return kOptions; }

const Option* FindOption(uint32_t id) {
  for (const Option& option : kOptions) {
    if (option.id == id) return &option;
  }
  return nullptr;
}

uint32_t VisibleOptionCount(Page page) {
  uint32_t count = 0;
  for (const Option& option : kOptions) {
    if (option.page == page && (!option.shown || option.shown())) ++count;
  }
  return count;
}

}  // namespace kameo::ui
