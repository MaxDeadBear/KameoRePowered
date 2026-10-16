#include "gpu/config.h"

REXCVAR_DEFINE_BOOL(kameo_native_renderer, true, "Kameo", "Use the native plume renderer instead of the xenos GPU plugin.");

REXCVAR_DEFINE_STRING(kameo_gfx_backend, "auto", "Kameo",
                      "Graphics backend: auto, d3d12 or vulkan. auto prefers D3D12 on Windows. "
                      "Running the same scene through both is the cheapest test of whether a "
                      "visual fault is in this renderer's logic (it will look identical on both) "
                      "or in one backend's path (it will not).");

REXCVAR_DEFINE_BOOL(kameo_gfx_widescreen, false, "Kameo", "Widen the field of view to the display's aspect instead of pillarboxing the 4:3-era 16:9 image. The scene projection is built by mtx44Perspective, which divides its X scale by the aspect it is handed, so substituting the display's shows more of the world at the sides rather than stretching it. The HUD is authored for 16:9 and stretches with it.");
REXCVAR_DEFINE_BOOL(kameo_gfx_shadows, true, "Kameo", "Draw shadows. Off leaves the shadow map at its cleared depth and skips the stencil shadow volumes, so nothing reads as shadowed.");
REXCVAR_DEFINE_BOOL(kameo_gfx_particles, true, "Kameo", "Draw particle effects (the PLite emitters and smoke plumes).");
REXCVAR_DEFINE_INT32(kameo_hud_layout, 2, "Kameo", "Where the HUD sits when the picture is wider than 16:9: 0 stretches it with the picture as the original did, 1 keeps its shape in a centred 16:9 area, 2 keeps its shape and moves each element to its own screen edge.");
REXCVAR_DEFINE_INT32(kameo_hud_scale, 100, "Kameo", "HUD size in percent, 50 - 150; each element grows or shrinks toward its own corner or edge.");
REXCVAR_DEFINE_INT32(kameo_gfx_shadow_resolution, 2, "Kameo", "Shadow map resolution multiplier: 1 is the original 1024 and 768 maps, 2 doubles them, 4 quadruples them.");
REXCVAR_DEFINE_INT32(kameo_gfx_user_gamma, 100, "Kameo", "Display gamma in percent, applied on top of the game's brightness: 100 leaves it unchanged, higher lifts the midtones, lower deepens them.");
REXCVAR_DEFINE_BOOL(kameo_gfx_normal_maps, true, "Kameo", "Experimental: sample the game's DXN normal maps. Off gives every such sample a flat normal, so surfaces light as smooth.");
REXCVAR_DEFINE_BOOL(kameo_gfx_bump_maps, true, "Kameo", "Experimental: apply the texbem bump-environment offsets (water, ice, oil and similar). Off zeroes their strength.");
REXCVAR_DEFINE_BOOL(kameo_gfx_specular, true, "Kameo", "Experimental: draw specular highlights. Off raises every shader's c_power exponent so the highlight term falls to zero.");
REXCVAR_DEFINE_BOOL(kameo_gfx_vsync, true, "Kameo", "Wait for the display's refresh before presenting. Off removes that frame rate ceiling and allows tearing.");
REXCVAR_DEFINE_BOOL(kameo_gfx_shadow_filtering, true, "Kameo", "Filter the shadow maps bilinearly so shadow edges come out smooth instead of in square texel steps.");
REXCVAR_DEFINE_INT32(kameo_gfx_fps_cap, 0, "Kameo", "Frame rate limit in frames per second, 0 for none.");
REXCVAR_DEFINE_BOOL(kameo_gfx_grass, true, "Kameo", "Draw the instanced grass.");
REXCVAR_DEFINE_BOOL(kameo_gfx_fur, true, "Kameo", "Draw the fur shells over furry models.");
REXCVAR_DEFINE_BOOL(kameo_gfx_dust_motes, true, "Kameo", "Draw the floating dust motes and sparkles that fill the air.");
REXCVAR_DEFINE_BOOL(kameo_gfx_bloom, true, "Kameo", "Draw the bloom glow and its composite.");
REXCVAR_DEFINE_BOOL(kameo_gfx_depth_of_field, true, "Kameo", "Blur the distance with the game's depth of field.");
REXCVAR_DEFINE_BOOL(kameo_gfx_heat_haze, true, "Kameo", "Draw heat haze distortion, both the screen pass and the billboards.");
REXCVAR_DEFINE_INT32(kameo_gfx_anisotropy, 0, "Kameo", "Anisotropic filtering forced onto trilinear samplers: 0 keeps the game's own level, otherwise 2, 4, 8 or 16.");
REXCVAR_DEFINE_INT32(kameo_gfx_render_height, 0, "Kameo", "Height in pixels to render the scene at, 0 to follow the window. 720 through 2160 (4K) are the usual choices; the width follows the window's aspect ratio, and the video, overlay and UI still draw at the window's own resolution.");
REXCVAR_DEFINE_INT32(kameo_gfx_render_width, 0, "Kameo", "Width in pixels to render the scene at alongside kameo_gfx_render_height, 0 to follow the window's aspect ratio; a size of another shape than the window is shown with black bars.");
REXCVAR_DEFINE_INT32(kameo_gfx_msaa, 4, "Kameo", "Multisample the scene with this many samples (2, 4 or 8; 0 or 1 for off). Besides smoothing geometry edges it lets alpha-to-mask foliage like the grass cover partial pixels the way the 360 did, instead of cutting its edges hard.");
REXCVAR_DEFINE_BOOL(kameo_gfx_gamma, true, "Kameo",
                    "Apply the guest's scanout gamma ramp at present. The 360 runs every "
                    "presented pixel through D3DDevice_SetGammaRamp in the display hardware; "
                    "we present to a plain UNORM swap chain, so without this the image is "
                    "measurably too dark (median 30 against the xenos reference's 55 at the "
                    "same guest frame). "
                    "This was briefly suspected of causing the Badlandz device removal and "
                    "defaulted off for it. That was WRONG: the freeze reproduces with the pass "
                    "disabled (gamma pass active: 0, DEVICE REMOVED: 1), so the correlation was "
                    "only that the earlier runs never reached that level. Do not re-disable it "
                    "on a correlation; the log says outright whether the pass was even built.");

REXCVAR_DEFINE_BOOL(kameo_gfx_preserve_aspect, true, "Kameo",
                    "Letterbox rather than stretch. The guest renders 16:9 and its "
                    "projection assumes it, so scaling X and Y independently to fill the "
                    "window distorts everything the moment the client area is not 16:9 -- "
                    "a 2576x1048 window stretches the picture horizontally by about 1.4x, "
                    "which reads as the camera being wrong rather than as the window being "
                    "the wrong shape. "
                    "Verified maximised: the scene is pillarboxed at 16:9 and the HUD sits "
                    "correctly inside the box. An earlier note here claimed the HUD kept its own "
                    "placement and the two disagreed -- that was a misreading of a screenshot "
                    "whose real problem was brightness, not geometry. There are only TWO "
                    "on-screen viewports in a frame, guest (0,0) 1280x720 for the scene AND for "
                    "the HUD, so they cannot diverge: both go through the same transform.");

REXCVAR_DEFINE_BOOL(kameo_gfx_capture_begin_vertices, true, "Kameo",
                    "Capture standalone D3DDevice_BeginVertices draws. This is the 2D icon "
                    "system -- health and energy bars, warrior button prompts, boss bars -- "
                    "and about 250 of them a frame used to be counted and then thrown away, "
                    "which is why the HUD had text but no bars and no button glyphs. Set "
                    "false to get the old behaviour back.");
REXCVAR_DEFINE_BOOL(kameo_gfx_merge_begin_vertices, true, "Kameo", "Fold consecutive BeginVertices list draws whose captured state is identical into one host draw.");
REXCVAR_DEFINE_BOOL(kameo_gfx_merge_indexed, true, "Kameo", "Fold consecutive indexed draws off one vertex buffer whose captured state is identical into one host draw.");
REXCVAR_DEFINE_INT32(kameo_gfx_fur_max_shells, 0, "Kameo", "Most fur shells a furry model draws; 0 keeps the game's own count.");
REXCVAR_DEFINE_BOOL(kameo_gfx_defer_vertices, true, "Kameo", "Read, hash and transcode bound vertex buffers on the render thread when it records the frame, as the console GPU reads them when it executes the draw, instead of on the guest thread at the draw call. The guest fences a buffer before rewriting it, and those fences stay pending until the frame is recorded. Off restores reading them at the draw call.");
REXCVAR_DEFINE_BOOL(kameo_gfx_constant_range, true, "Kameo", "Compare and copy only the constant registers a shader's microcode reads. False falls back to the whole 4 KB block, which is what to try if a shader renders with stale constants.");
REXCVAR_DEFINE_BOOL(kameo_gfx_guest_draw_packets, false, "Kameo", "Let the guest's D3D draw, BeginVertices, Resolve and ClearF also build the Xenos command-buffer packets the native renderer never reads; off saves the guest thread that work.");
REXCVAR_DEFINE_BOOL(kameo_gfx_pipeline_precache, true, "Kameo", "Prepare embedded pipeline recipes at startup to reduce first-use shader stalls.");
REXCVAR_DEFINE_BOOL(kameo_gfx_render_thread, true, "Kameo", "Record and present each frame on a dedicated render thread while the game runs its next frame; off records inside the game's own present.");

REXCVAR_DEFINE_DOUBLE(kameo_gfx_depth_bias_scale, 1.0, "Kameo",
                      "Multiplier on the guest's polygon offset. Nothing read the guest's depth "
                      "bias at all until now, so decals z-fought with the surface they sit on and "
                      "flickered. The guest offset is absolute normalised depth while D3D12's "
                      "DepthBias against a FLOAT depth target is scaled by 2^(zexp-23), so the "
                      "conversion is approximate; raise this if decals still shimmer, lower it if "
                      "they visibly float.");

REXCVAR_DEFINE_BOOL(kameo_gfx_dxn_swap, true, "Kameo",
                    "Swap the two BC4 sub-blocks of every DXN (xenos-fmt 49) texture on "
                    "upload. Xbox 360 DXN and DXGI BC5_UNORM both store two BC4 blocks per "
                    "4x4, but not in the same order, so mapping DXN straight onto BC5 hands "
                    "the shader a normal map with X and Y transposed -- light arrives from "
                    "the wrong direction per texel and skin reads as hard and metallic. Set "
                    "false to get the untransposed upload back for comparison.");

REXCVAR_DEFINE_BOOL(kameo_gfx_cull_both, true, "Kameo",
                  "Honour PA_SU_SC_MODE_CNTL with both cull bits set by dropping the draw. "
                  "Neither D3D12 nor plume has a FRONT_AND_BACK cull mode, so the old behaviour "
                  "mapped it to FRONT and drew the back faces of geometry the guest had removed. "
                  "Turn off to restore that.");

REXCVAR_DEFINE_BOOL(kameo_gfx_mipmaps, false, "Kameo",
                  "Upload the guest's own mip chain alongside level 0. Without it every texture "
                  "is a single level and minified surfaces alias into a dense per-pixel stipple "
                  "-- the speckled rock and ice. Only the unpacked levels are uploaded so far; "
                  "the packed tail (levels at or below 16 texels) is still missing.");

REXCVAR_DEFINE_INT32(kameo_gfx_stale_offscreen_frames, 2, "Kameo",
                     "How many frames old a pass may be and still answer a resolve whose surface "
                     "had no pass this frame. Unbounded reuse is what puts the warrior screen's "
                     "card frames over the main menu: the menu resolves that surface, nothing "
                     "rendered into it this frame, and the offscreen still holds the previous "
                     "screen. It was left unbounded to keep the storybook portraits from going "
                     "black -- but they are black anyway, so the case it protects is already "
                     "broken. -1 restores unbounded reuse.");

REXCVAR_DEFINE_BOOL(kameo_gfx_native_resolve, true, "Kameo",
                  "Allocate the full-frame scene resolve target at the window's resolution rather "
                  "than the guest's 1280x720. The post chain resolves the whole frame and "
                  "composites it back over the screen, so a guest-sized copy downscales the scene "
                  "to 720p and stretches it again -- which is why the image went soft the larger "
                  "the window got. Turn it off to compare, or if a pass that reads the copy with "
                  "guest-sized texel offsets looks wrong.");


namespace kameo::gpu {

bool NativeRendererEnabled() { return REXCVAR_GET(kameo_native_renderer); }

Backend NativeRendererBackend() {
  const std::string want = REXCVAR_GET(kameo_gfx_backend);
  if (want == "vulkan" || want == "vk") return Backend::Vulkan;
  if (want == "d3d12" || want == "dx12") return Backend::D3D12;
  return Backend::Auto;
}

}  // namespace kameo::gpu
