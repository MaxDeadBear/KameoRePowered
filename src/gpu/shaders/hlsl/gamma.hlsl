// Scanout gamma ramp.
//
// The Xbox 360 applies D3DDevice_SetGammaRamp at SCANOUT -- the display
// hardware runs every presented pixel through it, so it covers the scene, the
// HUD and Bink video alike. Our native path presents to a plain
// B8G8R8A8_UNORM swap chain (confirmed with RenderDoc: NOT _SRGB) and ignored
// the ramp completely, which left the whole image too dark.
//
// Kameo installs a real one. kamVideoGlobalGammaCorrection (0x820bf310) builds
//     ramp[i] = pow((i + 1) / 256, 1.5 - bias) * 65535
// and there is a user-facing gamma slider behind it (processGammaBar). Measured
// at runtime the curve is pow(x, 0.751): 1.72x at 12.5% input, 1.43x at 25%,
// 1.19x at 50%, tapering to 1.0 at white -- it lifts shadows and leaves
// highlights alone.
//
// Because the guest builds the ramp as a pure power curve, the exponent alone
// reproduces it exactly and no 256-entry LUT texture is needed; the exponent is
// recovered from the ramp the guest actually uploads rather than assumed.
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

[[vk::binding(0, 0)]] Texture2D<float4> g_Scene   : register(t0);
[[vk::binding(1, 0)]] SamplerState      g_Sampler : register(s1);

[[vk::binding(2, 0)]] cbuffer GammaConstants : register(b2) {
    float g_Exponent;   // 1.0 leaves the image untouched
    float g_Brightness; // the user's brightness, a multiplier after the curve
    float2 g_Padding;
};

float4 PSMain(VSOutput input) : SV_Target {
    const float3 colour = saturate(g_Scene.Sample(g_Sampler, input.uv).rgb);
    return float4(saturate(pow(colour, g_Exponent) * g_Brightness), 1.0);
}
