// What a shader mod's Shade function reads, and the calls a translated game shader makes to reach it.
// tools/shaders/build_shader_mod_source.py adds those calls to each shader (docs/modding.md).
#pragma once

struct ShadeInputs {
    float3 colour;        // the game's finished pixel
    float alpha;          // its opacity
    float3 albedo;        // the surface's colour before lighting, with all its layers; the lit pixel where it isn't lit
    float3 light;         // the light falling here, worked out from the two; 1 where the surface isn't lit
    float3 normal;        // the surface direction, camera space; (0, 0, 1) when has_normal is false
    float3 view;          // towards the camera, camera space
    float3 up;            // towards the sky, camera space
    float3 light_dir;     // towards the sun or main light, camera space
    float3 light_colour;  // that light's colour
    float2 uv;            // the first texture coordinate; (0, 0) in a shader with no textures
    float2 screen;        // the pixel's position, in pixels
    float time;           // seconds since the game started, wrapping hourly
    bool has_albedo;
    bool has_normal;
};

float3 Shade(ShadeInputs s);

// Every 2D texture read in the game's shaders goes through MOD_FETCH2D(texture, sampler, uv, offset), which is the
// game's tfetch2D unless a mod's shade.hlsl defines it first. A mod can read from somewhere else there, to make a
// surface flow or ripple; anything it doesn't mean to move should get tfetch2D's answer unchanged, as shadow maps and
// normal maps are read the same way. Where it's used, the shader's ModState is called mod, so the macro can pass on
// mod.s.time, the seconds since the game started:
//   #define MOD_FETCH2D(texture, sampler, uv, offset) MyFetch(texture, sampler, uv, offset, mod.s.time)

// Smooth value noise in 0..1 over a lattice of unit cells.
float ModNoise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    const float a = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453);
    const float b = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
    const float c = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7))) * 43758.5453);
    const float d = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// x from in_min..in_max to out_min..out_max, not clamped.
float3 ModRemap(float3 x, float3 in_min, float3 in_max, float3 out_min, float3 out_max) {
    return out_min + (x - in_min) / select(in_max - in_min == 0.0, 1e-5, in_max - in_min) * (out_max - out_min);
}

// The direction to a light placed towards the camera, up the sky, and to one side: the same in every scene.
float3 ModKeyDirection(ShadeInputs s, float towards_camera, float up, float side) {
    float3 across = cross(s.up, s.view);
    across *= rsqrt(max(dot(across, across), 1e-6));
    return normalize(s.view * towards_camera + s.up * up + across * side);
}

float ModKeyLight(ShadeInputs s, float towards_camera, float up, float side) {
    return dot(s.normal, ModKeyDirection(s, towards_camera, up, side));
}

// 0 below edge and 1 above, antialiased over one pixel's change.
float ModStep(float edge, float x) {
    const float w = max(fwidth(x), 1e-4);
    return smoothstep(edge - w, edge + w, x);
}

// x in 0..1 cut into flat steps.
float ModBands(float x, float bands) {
    const float n = max(bands, 1.0);
    return floor(saturate(x) * n + 0.0001) / max(n - 1.0, 1.0);
}

// 1 - N.V: the rim, 0 facing the camera and 1 at the silhouette.
float ModFresnel(ShadeInputs s) {
    return 1.0 - saturate(dot(s.normal, s.view));
}

// A comic halftone: 1 inside a dot whose size grows with amount (0..1), on a grid of cell pixels turned 45 degrees.
float ModHalftone(float2 screen, float amount, float cell) {
    const float2 p = float2(screen.x + screen.y, screen.x - screen.y) * 0.70710678 / max(cell, 1.0);
    const float2 f = frac(p) - 0.5;
    const float radius = sqrt(saturate(amount)) * 0.70710678;
    const float d = length(f);
    const float w = max(fwidth(d), 1e-4);
    return 1.0 - smoothstep(radius - w, radius + w, d);
}

// An inked outline: 1 on a line width pixels wide just inside the silhouette, where the surface turns away from the
// camera, and 0 elsewhere. How far a pixel is from that edge is how much it still faces the camera over how fast that
// changes across the screen, so the line keeps its width at any distance, as a drawn one does, and also runs round
// any part seen in front of another.
float ModOutline(ShadeInputs s, float width) {
    if (!s.has_normal) return 0.0;
    const float facing = saturate(dot(s.normal, s.view));
    const float pixels = facing / max(fwidth(facing), 1e-4);
    return 1.0 - smoothstep(width - 0.5, width + 0.5, pixels);
}

float ModLuminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// The per-channel forms the editor's nodes use, every value being a float3.
float3 ModStep3(float3 edge, float3 x) {
    return float3(ModStep(edge.x, x.x), ModStep(edge.y, x.y), ModStep(edge.z, x.z));
}

float3 ModBands3(float3 x, float3 bands) {
    return float3(ModBands(x.x, bands.x), ModBands(x.y, bands.y), ModBands(x.z, bands.z));
}

float3 ModDivide3(float3 a, float3 b) {
    return a / select(b == 0.0, 1e-5, b);
}

// What a shader's second run reads in place of its lighting constants, to leave only its textures: see
// tools/shaders/build_shader_mod_source.py. c_fog's y and c_shadowFade's y and w are the terms that turn fog and shade off.
static const float4 MOD_WHITE = float4(1.0, 1.0, 1.0, 1.0);
static const float4 MOD_BLACK = float4(0.0, 0.0, 0.0, 0.0);
static const float4 MOD_NO_FOG = float4(0.0, 1.0, 0.0, 0.0);
static const float4 MOD_NO_SHADE = float4(0.0, 1.0, 0.0, 1.0);

struct ModState {
    ShadeInputs s;
    bool lit;           // whether the game lights this surface at all; effects and glows it doesn't aren't passed to Shade
    bool derive_light;  // s.light is the lit colour over the albedo
    bool has_fog;
    float3 fog_colour;
    float fog_amount;   // how much of the surface shows through the fog
};

ModState ModStart(float2 screen, float time) {
    ModState m = (ModState)0;
    m.s.alpha = 1.0;
    m.s.light = float3(1.0, 1.0, 1.0);
    m.s.normal = float3(0.0, 0.0, 1.0);
    m.s.view = float3(0.0, 0.0, 1.0);
    m.s.up = float3(0.0, 1.0, 0.0);
    m.s.light_dir = float3(0.0, 0.0, 1.0);
    m.s.light_colour = float3(1.0, 1.0, 1.0);
    m.s.screen = screen;
    m.s.time = time;
    return m;
}

float3 ModNormalize(float3 v, float3 fallback) {
    const float length_squared = dot(v, v);
    return length_squared > 1e-8 ? v * rsqrt(length_squared) : fallback;
}

// The end of the shader, as they add to their colour after fog and would paint over a result given any earlier. A
// result that isn't a colour draws the game's, since a stray NaN from a divide would bloom across the screen.
// Surfaces the game doesn't light (glows, effects, and some surfaces lit from within, such as lava) are left alone
// unless the mod is built with MOD_INCLUDE_UNLIT; for those, colour is the finished pixel with any fog already in it,
// and there's no albedo, light or normal.
float3 ModFinish(inout ModState m, float3 colour, float alpha) {
#ifndef MOD_INCLUDE_UNLIT
    if (!m.lit) return colour;
#endif
    // The colour has the fog in it by now, and fog goes on again over the result, so it's taken out first: a pixel a
    // mod leaves alone then comes out as the game drew it, rather than fogged twice. Fully fogged, nothing shows.
    float3 clear = colour;
    if (m.has_fog) {
        if (m.fog_amount < 1.0 / 255.0) return colour;
        clear = m.fog_colour + (colour - m.fog_colour) / m.fog_amount;
    }
    if (!m.s.has_albedo) m.s.albedo = clear;
    m.s.colour = clear;
    m.s.alpha = alpha;
    if (m.derive_light) m.s.light = min(clear / max(m.s.albedo, 1.0 / 255.0), 8.0);
    const float3 result = Shade(m.s);
    if (!all(isfinite(result))) return colour;
    return m.has_fog ? lerp(m.fog_colour, max(result, 0.0), m.fog_amount) : max(result, 0.0);
}
