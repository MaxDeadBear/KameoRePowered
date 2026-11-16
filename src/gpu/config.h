#pragma once

// The renderer's player settings and the behaviour switches that are on by default.

#include <cstdint>
#include <string>

#include "core/cvar.h"

KCVAR_DECLARE(bool, kameo_native_renderer);
KCVAR_DECLARE(std::string, kameo_gfx_backend);
KCVAR_DECLARE(bool, kameo_gfx_widescreen);
KCVAR_DECLARE(bool, kameo_gfx_preserve_aspect);
KCVAR_DECLARE(int32_t, kameo_hud_layout);
KCVAR_DECLARE(int32_t, kameo_hud_scale);
KCVAR_DECLARE(int32_t, kameo_gfx_render_height);
KCVAR_DECLARE(int32_t, kameo_gfx_render_width);
KCVAR_DECLARE(int32_t, kameo_gfx_msaa);
KCVAR_DECLARE(bool, kameo_gfx_vsync);
KCVAR_DECLARE(int32_t, kameo_gfx_fps_cap);
KCVAR_DECLARE(bool, kameo_gfx_gamma);
KCVAR_DECLARE(int32_t, kameo_gfx_user_gamma);
KCVAR_DECLARE(bool, kameo_gfx_shadows);
KCVAR_DECLARE(int32_t, kameo_gfx_shadow_resolution);
KCVAR_DECLARE(bool, kameo_gfx_shadow_filtering);
KCVAR_DECLARE(bool, kameo_gfx_particles);
KCVAR_DECLARE(bool, kameo_gfx_grass);
KCVAR_DECLARE(bool, kameo_gfx_fur);
KCVAR_DECLARE(int32_t, kameo_gfx_fur_max_shells);
KCVAR_DECLARE(bool, kameo_gfx_dust_motes);
KCVAR_DECLARE(bool, kameo_gfx_bloom);
KCVAR_DECLARE(bool, kameo_gfx_depth_of_field);
KCVAR_DECLARE(bool, kameo_gfx_stable_geometry_skip);
KCVAR_DECLARE(std::string, kameo_dev_find_texture_shaders);
KCVAR_DECLARE(bool, kameo_dev_log_hud);
KCVAR_DECLARE(bool, kameo_gfx_heat_haze);
KCVAR_DECLARE(bool, kameo_gfx_normal_maps);
KCVAR_DECLARE(bool, kameo_gfx_bump_maps);
KCVAR_DECLARE(bool, kameo_gfx_specular);
KCVAR_DECLARE(int32_t, kameo_gfx_anisotropy);
KCVAR_DECLARE(bool, kameo_gfx_capture_begin_vertices);
KCVAR_DECLARE(bool, kameo_gfx_merge_begin_vertices);
KCVAR_DECLARE(bool, kameo_gfx_merge_indexed);
KCVAR_DECLARE(bool, kameo_gfx_defer_vertices);
KCVAR_DECLARE(bool, kameo_gfx_constant_range);
KCVAR_DECLARE(bool, kameo_gfx_guest_draw_packets);
KCVAR_DECLARE(double, kameo_gfx_depth_bias_scale);
KCVAR_DECLARE(bool, kameo_gfx_dxn_swap);
KCVAR_DECLARE(bool, kameo_gfx_cull_both);
KCVAR_DECLARE(bool, kameo_gfx_mipmaps);
KCVAR_DECLARE(int32_t, kameo_gfx_stale_offscreen_frames);
KCVAR_DECLARE(bool, kameo_gfx_native_resolve);
KCVAR_DECLARE(bool, kameo_gfx_pipeline_precache);
KCVAR_DECLARE(bool, kameo_gfx_render_thread);
KCVAR_DECLARE(bool, kameo_gfx_d3d12_debug);

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
