// The scene depth, cut to the guest frame's rectangle, into a single-sample R32F copy the guest's effects can sample; depth-stencil formats cannot be resolved by the API.

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

[[vk::binding(0, 0)]] Texture2DMS<float> g_DepthMS : register(t0);
[[vk::binding(0, 0)]] Texture2D<float> g_Depth : register(t0);

// Where the guest frame starts in the scene target, which pillarboxing or letterboxing moves off the origin.
[[vk::binding(1, 0)]] cbuffer DepthResolveConstants : register(b1) {
    int2 g_Origin;
    int2 g_Padding;
};

// The nearest sample, which is what a depth test against the resolved copy should see at an edge.
float PSMain(VSOutput input) : SV_Target {
    uint width, height, samples;
    g_DepthMS.GetDimensions(width, height, samples);
    const int2 texel = int2(input.position.xy) + g_Origin;
    float nearest = 1.0;
    for (uint i = 0; i < samples; ++i) nearest = min(nearest, g_DepthMS.Load(texel, i));
    return nearest;
}

float PSSingle(VSOutput input) : SV_Target {
    return g_Depth.Load(int3(int2(input.position.xy) + g_Origin, 0));
}
