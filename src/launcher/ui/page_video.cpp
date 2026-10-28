// Video page: display, graphics, effects, performance and camera options.

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

void PageVideo(App& app) {
  PageTitle(app, "Video", nullptr);
  RestoreButton(app, {"kameo_window_mode", "kameo_gfx_widescreen", "kameo_gfx_render_height", "kameo_gfx_render_width", "kameo_gfx_user_gamma", "kameo_hud_layout", "kameo_hud_scale", "kameo_gfx_msaa", "kameo_gfx_anisotropy", "kameo_gfx_shadows", "kameo_gfx_shadow_filtering", "kameo_gfx_shadow_resolution",
                      "kameo_gfx_particles", "kameo_gfx_grass", "kameo_gfx_fur", "kameo_gfx_dust_motes", "kameo_gfx_bloom", "kameo_gfx_depth_of_field", "kameo_gfx_heat_haze", "kameo_gfx_vsync", "kameo_fps_counter", "kameo_gfx_fps_cap", "kameo_camera_drift", "kameo_camera_auto_level", "kameo_camera_fov",
                      "kameo_gfx_normal_maps", "kameo_gfx_bump_maps", "kameo_gfx_specular"});
  if (BeginSection("display", "Display")) {
    DisplayModeRow(app);
    ResolutionRow(app);
    BoolRow(app, "Widescreen", "kameo_gfx_widescreen", "A wider view that fills the screen instead of the original 16:9 picture.");
    IntSliderRow(app, "Gamma", "kameo_gfx_user_gamma", 50, 200, "%d%%", "100% leaves the picture as the game draws it; higher brightens the midtones.");
    ChoiceRow(app, "HUD Layout", "kameo_hud_layout", {{"Stretched", 0}, {"Centred 16:9", 1}, {"Screen Edges", 2}});
    IntSliderRow(app, "HUD Scale", "kameo_hud_scale", 50, 150, "%d%%", "Each HUD element grows or shrinks toward its own corner or edge.");
  }
  EndSection();
  if (BeginSection("graphics", "Graphics")) {
    StringChoiceRow(app, "Renderer", "kameo_gfx_backend", {{"Automatic", "auto"}, {"Direct3D 12", "d3d12"}, {"Vulkan", "vulkan"}}, "Takes effect the next time the game starts. Automatic uses Direct3D 12 on Windows.");
    ChoiceRow(app, "Anti-aliasing", "kameo_gfx_msaa", {{"Off", 0}, {"2x", 2}, {"4x", 4}, {"8x", 8}});
    ChoiceRow(app, "Anisotropic Filtering", "kameo_gfx_anisotropy", {{"Game", 0}, {"2x", 2}, {"4x", 4}, {"8x", 8}, {"16x", 16}});
    BoolRow(app, "Shadows", "kameo_gfx_shadows");
    BoolChoiceRow(app, "Shadow Edges", "kameo_gfx_shadow_filtering", "Pixelated", "Smooth");
    ChoiceRow(app, "Shadow Quality", "kameo_gfx_shadow_resolution", {{"Original", 1}, {"High", 2}, {"Ultra", 4}});
  }
  EndSection();
  if (BeginSection("effects", "Effects")) {
    BoolRow(app, "Particles", "kameo_gfx_particles");
    BoolRow(app, "Grass", "kameo_gfx_grass");
    BoolRow(app, "Fur", "kameo_gfx_fur", "The layered fur on furry characters and costume trim.");
    BoolRow(app, "Dust Motes", "kameo_gfx_dust_motes", "The floating dust and sparkles that hang in the air.");
    BoolRow(app, "Bloom", "kameo_gfx_bloom");
    BoolRow(app, "Depth of Field", "kameo_gfx_depth_of_field");
    BoolRow(app, "Heat Haze", "kameo_gfx_heat_haze");
  }
  EndSection();
  if (BeginSection("performance", "Performance")) {
    BoolRow(app, "VSync", "kameo_gfx_vsync", "Wait for the display's refresh. Off uncaps the frame rate but may tear.");
    BoolRow(app, "FPS Counter", "kameo_fps_counter");
    ChoiceRow(app, "FPS Limit", "kameo_gfx_fps_cap", {{"Off", 0}, {"30", 30}, {"60", 60}, {"120", 120}, {"144", 144}, {"165", 165}, {"240", 240}});
  }
  EndSection();
  if (BeginSection("camera", "Camera")) {
    BoolChoiceRow(app, "Camera Follow", "kameo_camera_drift", "Fixed Distance", "Original (drifts)", "The original camera falls further behind and higher the longer you run.");
    BoolRow(app, "Auto-Level", "kameo_camera_auto_level", "Tilt the camera back to its default angle while you move, as the original game did.");
    IntSliderRow(app, "Field of View", "kameo_camera_fov", -10, 30, "%+d", "Degrees added to the gameplay camera's field of view. Cutscenes keep their own.");
  }
  EndSection();
  if (BeginSection("experimental", "Experimental")) {
    BoolRow(app, "Normal Maps", "kameo_gfx_normal_maps");
    BoolRow(app, "Bump Maps", "kameo_gfx_bump_maps");
    BoolRow(app, "Specular", "kameo_gfx_specular");
  }
  EndSection();
}

}  // namespace kameo::launcher
