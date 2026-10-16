// Scaling resolve blit.
//
// CopyTextureRegion CROPS; it cannot scale. The scene renders at the window's
// size while the guest's resolve targets are created at the guest's own
// dimensions, so a straight copy takes the top-left corner of the image and
// everything that samples the result shows a zoomed fragment. That is invisible
// while the client area happens to be 1280x720 and wrong at every other size.
//
// The same applies to the downsample chain, which shrinks 512x512 -> 426x240 ->
// 160x90 -> 32x32 and was likewise taking corners rather than filtering.
//
// Compiled offline by tools/shaders/build_builtin_shaders.py.

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Full-screen triangle from the vertex id -- no vertex buffer, no input layout.
VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> g_Source  : register(t0);
[[vk::binding(1, 0)]] SamplerState      g_Sampler : register(s1);

// The source rectangle, normalised: xy = top-left, zw = size. A resolve may name
// a sub-rectangle of the source, so the triangle's uv is remapped onto it.
[[vk::binding(2, 0)]] cbuffer BlitConstants : register(b2) {
    float4 g_SourceRect;
};

float4 PSMain(VSOutput input) : SV_Target {
    return g_Source.Sample(g_Sampler, g_SourceRect.xy + input.uv * g_SourceRect.zw);
}
