// The ImGui UI blit (gpu/ui/ui_renderer.cpp).
//
// The vertices arrive already in clip space, so the vertex shader passes them
// through and needs no constants.
//
// Binding numbers are FLAT across descriptor types in plume and the D3D
// register index must equal the binding number. The [[vk::binding(N, 0)]]
// attributes give SPIR-V the same numbering.
//
// Compiled offline by tools/shaders/build_builtin_shaders.py.

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 colour   : COLOR0;
};

VSOutput VSMain(float2 position : POSITION, float2 uv : TEXCOORD0, float4 colour : COLOR0) {
    VSOutput output;
    output.position = float4(position, 0.0, 1.0);
    output.uv = uv;
    output.colour = colour;
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> g_Atlas   : register(t0);
[[vk::binding(1, 0)]] SamplerState      g_Sampler : register(s1);

// ImGui's font atlas and the achievement icons are full RGBA, so the sample
// carries colour as well as coverage.
float4 PSMainRGBA(VSOutput input) : SV_Target {
    return input.colour * g_Atlas.Sample(g_Sampler, input.uv);
}
