// Bink video blit: YUV(+alpha) planes -> RGB, as a screen-space quad.
//
// Replaces Kameo's own Draw_Bink_textures (0x82265558). The game decodes Bink
// frames on the CPU into three or four D3DFMT_LIN_L8 planes; we sample those
// directly and do the colour conversion here.
//
// The conversion matrix is NOT baked in: it is read from the guest's own
// `yuvtorgb` globals (0x8273A5B0, four float4s) and uploaded, so the output
// matches what the title shipped with rather than a coefficient set we picked.
//
// Binding numbers are FLAT across descriptor types in plume, and the D3D
// register index must equal the binding number (a sampler at binding 4 is
// register(s4), not s0). The [[vk::binding(N, 0)]] attributes give SPIR-V the
// same numbering.
//
// Compiled offline by scripts/build_gfx_shaders.py into bink_shaders.h.

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Positions arrive already in clip space -- the pixel-to-clip conversion is done
// on the CPU when the quad is built, which keeps the vertex shader free of any
// constant buffer.
VSOutput VSMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    VSOutput output;
    output.position = float4(position, 1.0);
    output.uv = uv;
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> g_TextureY  : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> g_TextureCr : register(t1);
[[vk::binding(2, 0)]] Texture2D<float4> g_TextureCb : register(t2);
[[vk::binding(3, 0)]] Texture2D<float4> g_TextureA  : register(t3);
[[vk::binding(4, 0)]] SamplerState      g_Sampler   : register(s4);

[[vk::binding(5, 0)]] cbuffer BinkConstants : register(b5) {
    float4 g_YuvToRgb[4];   // verbatim from the guest's `yuvtorgb`
    uint   g_HasAlpha;
    uint3  g_Padding;
};

float4 PSMain(VSOutput input) : SV_Target {
    // Planes are L8, so the value lands in .r.
    float y  = g_TextureY.Sample(g_Sampler, input.uv).r;
    float cr = g_TextureCr.Sample(g_Sampler, input.uv).r;
    float cb = g_TextureCb.Sample(g_Sampler, input.uv).r;

    // Same shape the Xbox pixel shader used: a matrix applied to (y, cr, cb, 1).
    float4 yuv = float4(y, cr, cb, 1.0);
    float3 rgb;
    rgb.r = dot(yuv, g_YuvToRgb[0]);
    rgb.g = dot(yuv, g_YuvToRgb[1]);
    rgb.b = dot(yuv, g_YuvToRgb[2]);

    float alpha = (g_HasAlpha != 0) ? g_TextureA.Sample(g_Sampler, input.uv).r : 1.0;
    return float4(saturate(rgb), alpha);
}
