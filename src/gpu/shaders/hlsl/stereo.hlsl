// Stereoscopic composite.
//
// The scene is drawn once per eye; the first eye is copied aside and the
// second stays in the scene target. This pass puts the two onto the swap chain
// in the shape the display wants: each eye squeezed into its half (side by side
// or top and bottom, the formats 3D TVs and side-by-side headsets take), or
// both mixed into one red/cyan anaglyph image.
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

[[vk::binding(0, 0)]] Texture2D<float4> g_Left    : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> g_Right   : register(t1);
[[vk::binding(2, 0)]] SamplerState      g_Sampler : register(s2);

[[vk::binding(3, 0)]] cbuffer StereoConstants : register(b3) {
    uint g_Mode;  // 1 side by side, 2 top and bottom, 3 anaglyph
    uint3 g_Padding;
};

float4 PSMain(VSOutput input) : SV_Target {
    const float2 uv = input.uv;
    if (g_Mode == 1) {
        return uv.x < 0.5 ? g_Left.Sample(g_Sampler, float2(uv.x * 2.0, uv.y)) : g_Right.Sample(g_Sampler, float2(uv.x * 2.0 - 1.0, uv.y));
    }
    if (g_Mode == 2) {
        return uv.y < 0.5 ? g_Left.Sample(g_Sampler, float2(uv.x, uv.y * 2.0)) : g_Right.Sample(g_Sampler, float2(uv.x, uv.y * 2.0 - 1.0));
    }
    // Dubois' least-squares red/cyan anaglyph: keeps more of the colour and less of the ghosting than a plain red = left, cyan = right split.
    const float3 l = saturate(g_Left.Sample(g_Sampler, uv).rgb);
    const float3 r = saturate(g_Right.Sample(g_Sampler, uv).rgb);
    const float3x3 left_matrix = float3x3(0.437, 0.449, 0.164,
                                          -0.062, -0.062, -0.024,
                                          -0.048, -0.050, -0.017);
    const float3x3 right_matrix = float3x3(-0.011, -0.032, -0.007,
                                           0.377, 0.761, 0.009,
                                           -0.026, -0.093, 1.234);
    return float4(saturate(mul(left_matrix, l) + mul(right_matrix, r)), 1.0);
}
