#pragma once

// The renderer's player settings and the behaviour switches that are on by default.

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, kameo_native_renderer);
REXCVAR_DECLARE(std::string, kameo_gfx_backend);
REXCVAR_DECLARE(bool, kameo_gfx_widescreen);
REXCVAR_DECLARE(bool, kameo_gfx_preserve_aspect);
REXCVAR_DECLARE(int32_t, kameo_hud_layout);
REXCVAR_DECLARE(int32_t, kameo_hud_scale);
REXCVAR_DECLARE(int32_t, kameo_gfx_render_height);
REXCVAR_DECLARE(int32_t, kameo_gfx_render_width);
REXCVAR_DECLARE(int32_t, kameo_gfx_msaa);
REXCVAR_DECLARE(bool, kameo_gfx_vsync);
REXCVAR_DECLARE(int32_t, kameo_gfx_fps_cap);
REXCVAR_DECLARE(bool, kameo_gfx_gamma);
REXCVAR_DECLARE(int32_t, kameo_gfx_user_gamma);
REXCVAR_DECLARE(bool, kameo_gfx_shadows);
REXCVAR_DECLARE(int32_t, kameo_gfx_shadow_resolution);
REXCVAR_DECLARE(bool, kameo_gfx_shadow_filtering);
REXCVAR_DECLARE(bool, kameo_gfx_particles);
REXCVAR_DECLARE(bool, kameo_gfx_grass);
REXCVAR_DECLARE(bool, kameo_gfx_fur);
REXCVAR_DECLARE(int32_t, kameo_gfx_fur_max_shells);
REXCVAR_DECLARE(bool, kameo_gfx_dust_motes);
REXCVAR_DECLARE(bool, kameo_gfx_bloom);
REXCVAR_DECLARE(bool, kameo_gfx_depth_of_field);
REXCVAR_DECLARE(bool, kameo_gfx_heat_haze);
REXCVAR_DECLARE(bool, kameo_gfx_normal_maps);
REXCVAR_DECLARE(bool, kameo_gfx_bump_maps);
REXCVAR_DECLARE(bool, kameo_gfx_specular);
REXCVAR_DECLARE(int32_t, kameo_gfx_anisotropy);
REXCVAR_DECLARE(bool, kameo_gfx_capture_begin_vertices);
REXCVAR_DECLARE(bool, kameo_gfx_merge_begin_vertices);
REXCVAR_DECLARE(bool, kameo_gfx_merge_indexed);
REXCVAR_DECLARE(bool, kameo_gfx_defer_vertices);
REXCVAR_DECLARE(bool, kameo_gfx_constant_range);
REXCVAR_DECLARE(bool, kameo_gfx_guest_draw_packets);
REXCVAR_DECLARE(double, kameo_gfx_depth_bias_scale);
REXCVAR_DECLARE(bool, kameo_gfx_dxn_swap);
REXCVAR_DECLARE(bool, kameo_gfx_cull_both);
REXCVAR_DECLARE(bool, kameo_gfx_mipmaps);
REXCVAR_DECLARE(int32_t, kameo_gfx_stale_offscreen_frames);
REXCVAR_DECLARE(bool, kameo_gfx_native_resolve);
REXCVAR_DECLARE(bool, kameo_gfx_pipeline_precache);
REXCVAR_DECLARE(bool, kameo_gfx_render_thread);

namespace kameo::gpu {

enum class Backend {
  Auto,  // D3D12 on Windows, Vulkan elsewhere
  D3D12,
  Vulkan,
};

// Reads kameo_native_renderer; false falls back to the xenos plugin as a reference renderer.
bool NativeRendererEnabled();

// Reads kameo_gfx_backend; running one frame through both backends separates a bug in this renderer from one in plume's D3D12 or Vulkan path.
Backend NativeRendererBackend();

}  // namespace kameo::gpu
