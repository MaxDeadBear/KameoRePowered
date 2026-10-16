// The settings kameo_settings.toml holds, shared by the game's pause menu and the launcher so both read and write the same file the same way.

#pragma once

#include <functional>
#include <string>

enum class KameoSettingType { Bool, Int, Double, String };

struct KameoSettingSpec {
  const char* heading;
  const char* name;
  const char* help;
  KameoSettingType type;
  // The value the game plays with when the file leaves the setting out, after its own PC defaults are layered over the runtime's.
  const char* default_value;
};

inline constexpr KameoSettingSpec kKameoSettings[] = {
    {"Display", "kameo_window_mode", "0 = Windowed, 1 = Borderless, 2 = Fullscreen (exclusive). Use 0 or 1 if fullscreen misbehaves.", KameoSettingType::Int, "-1"},
    {nullptr, "kameo_gfx_widescreen", "true = wider view filling the screen, false = the original 16:9 picture.", KameoSettingType::Bool, "false"},
    {nullptr, "kameo_gfx_user_gamma", "Gamma in percent: 100 = unchanged, higher = brighter midtones, 50 - 200.", KameoSettingType::Int, "100"},
    {nullptr, "kameo_hud_layout", "HUD on screens wider than 16:9: 0 = stretched, 1 = centred 16:9, 2 = each element at its screen edge.", KameoSettingType::Int, "2"},
    {nullptr, "kameo_hud_scale", "HUD size in percent, 50 - 150.", KameoSettingType::Int, "100"},
    {nullptr, "kameo_gfx_render_height", "Render height: 0 = match the window, or e.g. 720, 1080, 1440, 2160.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_gfx_render_width", "Render width to go with the height, e.g. 1920 or 3440; 0 = follow the window's shape.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_gfx_backend", "Renderer: d3d12 or vulkan; auto = Direct3D 12 on Windows. Takes effect the next time the game starts.", KameoSettingType::String, "auto"},
    {nullptr, "kameo_gfx_msaa", "Anti-aliasing samples: 0 = off, 2, 4 or 8.", KameoSettingType::Int, "4"},
    {nullptr, "kameo_gfx_anisotropy", "Anisotropic filtering: 0 = game default, 2, 4, 8 or 16.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_gfx_vsync", "true waits for the display refresh; false uncaps the frame rate (may tear).", KameoSettingType::Bool, "true"},
    {nullptr, "kameo_fps_counter", "true shows the frame rate in the top-right corner.", KameoSettingType::Bool, "false"},
    {nullptr, "kameo_gfx_fps_cap", "Frame rate limit: 0 = off, or e.g. 30, 60, 120, 144, 165, 240.", KameoSettingType::Int, "0"},
    {"Effects (true or false)", "kameo_gfx_shadows", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_shadow_filtering", "true smooths shadow edges, false keeps the original square texels.", KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_shadow_resolution", "Shadow quality (a number, not true/false): 1 = original, 2 = high, 4 = ultra.", KameoSettingType::Int, "2"},
    {nullptr, "kameo_gfx_particles", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_grass", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_fur", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_dust_motes", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_bloom", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_depth_of_field", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_heat_haze", nullptr, KameoSettingType::Bool, "true"},
    {"Camera", "kameo_camera_drift", "false keeps the follow camera at its default distance; true restores the original camera that drifts further back the longer you run.", KameoSettingType::Bool, "false"},
    {nullptr, "kameo_camera_auto_level", "true lets the camera tilt itself back to its default angle while you move (the original behaviour).", KameoSettingType::Bool, "false"},
    {nullptr, "kameo_camera_fov", "Degrees added to the field of view in gameplay, -10 to 30 (0 = the game's own).", KameoSettingType::Int, "0"},
    {"Experimental (true or false)", "kameo_gfx_normal_maps", "Normal maps: false lights surfaces as smooth.", KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_bump_maps", "Bump maps (water, ice and similar distortion): false flattens them.", KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_specular", "Specular highlights: false removes the shine.", KameoSettingType::Bool, "true"},
    {"Audio (0 - 100)", "kameo_volume_master", nullptr, KameoSettingType::Int, "50"},
    {nullptr, "kameo_volume_music", nullptr, KameoSettingType::Int, "50"},
    {nullptr, "kameo_volume_sfx", nullptr, KameoSettingType::Int, "50"},
    {nullptr, "kameo_volume_ambience", nullptr, KameoSettingType::Int, "50"},
    {nullptr, "kameo_volume_speech", nullptr, KameoSettingType::Int, "50"},
    {nullptr, "kameo_volume_cutscenes", nullptr, KameoSettingType::Int, "50"},
    {"Language (takes effect on the next launch)", "kameo_language", "0 = keep user_language, 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Traditional Chinese.", KameoSettingType::Int, "0"},
    {"Mods (true or false)", "kameo_gfx_texture_replacements", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_gfx_dump_textures", nullptr, KameoSettingType::Bool, "true"},
    {nullptr, "kameo_movie_replacements", "Play movies/<name>.mp4 in place of fmv/<name>.bik. With a soundtrack it plays its own sound for its own length; without one only the picture is replaced.", KameoSettingType::Bool, "true"},
    {nullptr, "kameo_story_splitscreen", nullptr, KameoSettingType::Bool, "false"},
    {nullptr, "kameo_unlock_all_extras", nullptr, KameoSettingType::Bool, "false"},
    {nullptr, "kameo_unlock_all_warriors", nullptr, KameoSettingType::Bool, "false"},
    {nullptr, "kameo_unlocked_codes", "Features unlocked by pause-menu cheat codes, one bit each.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_skip_cutscenes", "Start skips in-engine cutscenes and every FMV.", KameoSettingType::Bool, "true"},
    {"Controls", "kameo_input_mode", "Player one: 0 = keyboard and mouse, 1 = first available gamepad.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_p2_input_mode", "0 = Co-op gamepad, 1 = Co-op keyboard and mouse (a second physical keyboard is needed when player one also uses one).", KameoSettingType::Int, "0"},
    {nullptr, "kameo_p1_keyboard_device", "Player one physical keyboard: 0 = Keyboard 1, 1 = Keyboard 2.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_p2_keyboard_device", "Co-op physical keyboard: 0 = Keyboard 1, 1 = Keyboard 2.", KameoSettingType::Int, "1"},
    {nullptr, "kameo_p1_mouse_device", "Player one physical mouse: 0 = Mouse 1, 1 = Mouse 2.", KameoSettingType::Int, "0"},
    {nullptr, "kameo_p2_mouse_device", "Co-op physical mouse: 0 = Mouse 1, 1 = Mouse 2.", KameoSettingType::Int, "1"},
    {nullptr, "mnk_mouse", "true = the mouse turns the camera in gameplay (menus always get a pointer).", KameoSettingType::Bool, "true"},
    {nullptr, "mnk_sensitivity", "Mouse camera sensitivity, 0.1 - 10.0.", KameoSettingType::Double, "1.0"},
    {"Controller bindings (physical input for each game control)", "padbind_a", "A action.", KameoSettingType::String, "face_bottom"},
    {nullptr, "padbind_b", "B action.", KameoSettingType::String, "face_right"},
    {nullptr, "padbind_x", "X action.", KameoSettingType::String, "face_left"},
    {nullptr, "padbind_y", "Y action.", KameoSettingType::String, "face_top"},
    {nullptr, "padbind_left_shoulder", nullptr, KameoSettingType::String, "left_shoulder"},
    {nullptr, "padbind_right_shoulder", nullptr, KameoSettingType::String, "right_shoulder"},
    {nullptr, "padbind_left_trigger", nullptr, KameoSettingType::String, "left_trigger"},
    {nullptr, "padbind_right_trigger", nullptr, KameoSettingType::String, "right_trigger"},
    {nullptr, "padbind_left_stick_press", nullptr, KameoSettingType::String, "left_stick_press"},
    {nullptr, "padbind_right_stick_press", nullptr, KameoSettingType::String, "right_stick_press"},
    {nullptr, "padbind_dpad_up", nullptr, KameoSettingType::String, "dpad_up"},
    {nullptr, "padbind_dpad_down", nullptr, KameoSettingType::String, "dpad_down"},
    {nullptr, "padbind_dpad_left", nullptr, KameoSettingType::String, "dpad_left"},
    {nullptr, "padbind_dpad_right", nullptr, KameoSettingType::String, "dpad_right"},
    {nullptr, "padbind_start", nullptr, KameoSettingType::String, "start"},
    {nullptr, "padbind_back", nullptr, KameoSettingType::String, "back"},
    {nullptr, "padbind_left_up", nullptr, KameoSettingType::String, "left_up"},
    {nullptr, "padbind_left_down", nullptr, KameoSettingType::String, "left_down"},
    {nullptr, "padbind_left_left", nullptr, KameoSettingType::String, "left_left"},
    {nullptr, "padbind_left_right", nullptr, KameoSettingType::String, "left_right"},
    {nullptr, "padbind_right_up", nullptr, KameoSettingType::String, "right_up"},
    {nullptr, "padbind_right_down", nullptr, KameoSettingType::String, "right_down"},
    {nullptr, "padbind_right_left", nullptr, KameoSettingType::String, "right_left"},
    {nullptr, "padbind_right_right", nullptr, KameoSettingType::String, "right_right"},
    {"Key bindings (key names: A-Z, 0-9, Space, Return, Escape, Tab, Backspace, Up, Down, Left, Right, LMB, RMB, MMB, F1-F12, Numpad0-9; separate several keys with commas)", "keybind_a", "A button", KameoSettingType::String, "Space,Return"},
    {nullptr, "keybind_b", "B button", KameoSettingType::String, "R,Backspace"},
    {nullptr, "keybind_x", "X button", KameoSettingType::String, "Q"},
    {nullptr, "keybind_y", "Y button", KameoSettingType::String, "E"},
    {nullptr, "keybind_left_trigger", "Left trigger", KameoSettingType::String, "LMB"},
    {nullptr, "keybind_right_trigger", "Right trigger", KameoSettingType::String, "RMB"},
    {nullptr, "keybind_left_shoulder", "Left bumper", KameoSettingType::String, "F"},
    {nullptr, "keybind_right_shoulder", "Right bumper", KameoSettingType::String, "G"},
    {nullptr, "keybind_lstick_up", "Move forward", KameoSettingType::String, "W"},
    {nullptr, "keybind_lstick_down", "Move back", KameoSettingType::String, "S"},
    {nullptr, "keybind_lstick_left", "Move left", KameoSettingType::String, "A"},
    {nullptr, "keybind_lstick_right", "Move right", KameoSettingType::String, "D"},
    {nullptr, "keybind_lstick_press", "Left stick click", KameoSettingType::String, "V"},
    {nullptr, "keybind_rstick_up", "Camera up", KameoSettingType::String, "I"},
    {nullptr, "keybind_rstick_down", "Camera down", KameoSettingType::String, "K"},
    {nullptr, "keybind_rstick_left", "Camera left", KameoSettingType::String, "J"},
    {nullptr, "keybind_rstick_right", "Camera right", KameoSettingType::String, "L"},
    {nullptr, "keybind_rstick_press", "Right stick click", KameoSettingType::String, "MMB"},
    {nullptr, "keybind_dpad_up", "D-pad up", KameoSettingType::String, "Up"},
    {nullptr, "keybind_dpad_down", "D-pad down", KameoSettingType::String, "Down"},
    {nullptr, "keybind_dpad_left", "D-pad left", KameoSettingType::String, "Left"},
    {nullptr, "keybind_dpad_right", "D-pad right", KameoSettingType::String, "Right"},
    {nullptr, "keybind_back", "Back button", KameoSettingType::String, "Tab"},
    {nullptr, "keybind_start", "Start button", KameoSettingType::String, "Escape"},
};

inline const KameoSettingSpec* KameoFindSetting(const std::string& name) {
  for (const KameoSettingSpec& spec : kKameoSettings) {
    if (name == spec.name) return &spec;
  }
  return nullptr;
}

// The whole file, one line per setting under its heading and help comment; `value_of` returns a setting's value as TOML would spell it, minus the quotes strings get here.
inline std::string KameoFormatSettingsFile(const std::function<bool(const KameoSettingSpec&, std::string&)>& value_of) {
  std::string text;
  text += "# Kameo RePowered settings, written by the pause menu and the launcher whenever an option changes.\n";
  text += "# Edit any value while the game is closed; delete a line (or this file) to go back to the default.\n";
  text += "# Values here override kameorepowered.toml, and a --flag on the command line overrides both.\n";
  for (const KameoSettingSpec& spec : kKameoSettings) {
    std::string value;
    if (!value_of(spec, value)) continue;
    if (spec.heading) text += std::string("\n# --- ") + spec.heading + " ---\n";
    if (spec.help) text += std::string("# ") + spec.help + "\n";
    if (spec.type == KameoSettingType::String) {
      std::string quoted = "\"";
      for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
      }
      value = quoted + "\"";
    }
    text += std::string(spec.name) + " = " + value + "\n";
  }
  return text;
}
