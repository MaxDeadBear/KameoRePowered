// 2D overlay (title-screen text) blit.
//
// Kameo draws its UI text as one quad per glyph through D3DDevice_DrawVerticesUP
// -- measured at ~148 draws/frame on the title screen, all sharing a single
// font atlas. The guest vertices are PRE-TRANSFORMED (float4 position with
// rhw=1.0, x/y in pixels), so no vertex shader work and no constants are
// needed: the pixel-to-clip conversion happens on the CPU when the batch is
// built, exactly as the Bink blit does.
//
// The atlas is Xenos format 2 (k_8) -- a single 8-bit channel, which is glyph
// coverage, not colour. So the texture supplies alpha and the vertex colour
// supplies RGB.
//
// Binding numbers are FLAT across descriptor types in plume and the D3D
// register index must equal the binding number. The [[vk::binding(N, 0)]]
// attributes give SPIR-V the same numbering.
//
// Compiled offline by scripts/build_gfx_shaders.py.

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

float4 PSMain(VSOutput input) : SV_Target {
    // k_8 lands in .r and is coverage.
    float coverage = g_Atlas.Sample(g_Sampler, input.uv).r;
    return float4(input.colour.rgb, input.colour.a * coverage);
}
