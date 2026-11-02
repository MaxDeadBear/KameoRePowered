// Video page: display, graphics, effects, performance and camera options.

#include "core/localization.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

using l10n::Tr;

void PageVideo(App& app) {
  PageTitle(app, Tr("Video"), nullptr);
  RestoreButton(app, {"kameo_window_mode", "kameo_gfx_widescreen", "kameo_gfx_render_height", "kameo_gfx_render_width", "kameo_gfx_user_gamma", "kameo_hud_layout", "kameo_hud_scale", "kameo_gfx_msaa", "kameo_gfx_anisotropy", "kameo_gfx_shadows", "kameo_gfx_shadow_filtering", "kameo_gfx_shadow_resolution",
                      "kameo_gfx_particles", "kameo_gfx_grass", "kameo_gfx_fur", "kameo_gfx_dust_motes", "kameo_gfx_bloom", "kameo_gfx_depth_of_field", "kameo_gfx_heat_haze", "kameo_gfx_vsync", "kameo_fps_counter", "kameo_gfx_fps_cap", "kameo_camera_drift", "kameo_camera_auto_level", "kameo_camera_fov",
                      "kameo_gfx_normal_maps", "kameo_gfx_bump_maps", "kameo_gfx_specular"});
  if (BeginSection("display", Tr("Display"))) {
    DisplayModeRow(app);
    ResolutionRow(app);
    BoolRow(app, Tr("Widescreen"), "kameo_gfx_widescreen", Tr("A wider view that fills the screen instead of the original 16:9 picture."));
    IntSliderRow(app, Tr("Gamma"), "kameo_gfx_user_gamma", 50, 200, "%d%%", Tr("100% leaves the picture as the game draws it; higher brightens the midtones."));
    ChoiceRow(app, Tr("HUD Layout"), "kameo_hud_layout", {{Tr("Stretched"), 0}, {Tr("Centred 16:9"), 1}, {Tr("Screen Edges"), 2}});
    IntSliderRow(app, Tr("HUD Scale"), "kameo_hud_scale", 50, 150, "%d%%", Tr("Each HUD element grows or shrinks toward its own corner or edge."));
  }
  EndSection();
  if (BeginSection("graphics", Tr("Graphics"))) {
    StringChoiceRow(app, Tr("Renderer"), "kameo_gfx_backend", {{Tr("Automatic"), "auto"}, {"Direct3D 12", "d3d12"}, {"Vulkan", "vulkan"}}, Tr("Takes effect the next time the game starts. Automatic uses Direct3D 12 on Windows."));
    ChoiceRow(app, Tr("Anti-aliasing"), "kameo_gfx_msaa", {{Tr("Off"), 0}, {"2x", 2}, {"4x", 4}, {"8x", 8}});
    ChoiceRow(app, Tr("Anisotropic Filtering"), "kameo_gfx_anisotropy", {{Tr("Game"), 0}, {"2x", 2}, {"4x", 4}, {"8x", 8}, {"16x", 16}});
    BoolRow(app, Tr("Shadows"), "kameo_gfx_shadows");
    BoolChoiceRow(app, Tr("Shadow Edges"), "kameo_gfx_shadow_filtering", Tr("Pixelated"), Tr("Smooth"));
    ChoiceRow(app, Tr("Shadow Quality"), "kameo_gfx_shadow_resolution", {{Tr("Original"), 1}, {Tr("High"), 2}, {Tr("Ultra"), 4}});
  }
  EndSection();
  if (BeginSection("effects", Tr("Effects"))) {
    BoolRow(app, Tr("Particles"), "kameo_gfx_particles");
    BoolRow(app, Tr("Grass"), "kameo_gfx_grass");
    BoolRow(app, Tr("Fur"), "kameo_gfx_fur", Tr("The layered fur on furry characters and costume trim."));
    BoolRow(app, Tr("Dust Motes"), "kameo_gfx_dust_motes", Tr("The floating dust and sparkles that hang in the air."));
    BoolRow(app, Tr("Bloom"), "kameo_gfx_bloom");
    BoolRow(app, Tr("Depth of Field"), "kameo_gfx_depth_of_field");
    BoolRow(app, Tr("Heat Haze"), "kameo_gfx_heat_haze");
  }
  EndSection();
  if (BeginSection("performance", Tr("Performance"))) {
    BoolRow(app, Tr("VSync"), "kameo_gfx_vsync", Tr("Wait for the display's refresh. Off uncaps the frame rate but may tear."));
    BoolRow(app, Tr("FPS Counter"), "kameo_fps_counter");
    ChoiceRow(app, Tr("FPS Limit"), "kameo_gfx_fps_cap", {{Tr("Off"), 0}, {"30", 30}, {"60", 60}, {"120", 120}, {"144", 144}, {"165", 165}, {"240", 240}});
  }
  EndSection();
  if (BeginSection("camera", Tr("Camera"))) {
    BoolChoiceRow(app, Tr("Camera Follow"), "kameo_camera_drift", Tr("Fixed Distance"), Tr("Original (drifts)"), Tr("The original camera falls further behind and higher the longer you run."));
    BoolRow(app, Tr("Auto-Level"), "kameo_camera_auto_level", Tr("Tilt the camera back to its default angle while you move, as the original game did."));
    IntSliderRow(app, Tr("Field of View"), "kameo_camera_fov", -10, 30, "%+d", Tr("Degrees added to the gameplay camera's field of view. Cutscenes keep their own."));
  }
  EndSection();
  if (BeginSection("experimental", Tr("Experimental"))) {
    BoolRow(app, Tr("Normal Maps"), "kameo_gfx_normal_maps");
    BoolRow(app, Tr("Bump Maps"), "kameo_gfx_bump_maps");
    BoolRow(app, Tr("Specular"), "kameo_gfx_specular");
  }
  EndSection();
}

}  // namespace kameo::launcher
