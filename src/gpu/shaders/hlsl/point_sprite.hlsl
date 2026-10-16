#ifdef __spirv__
struct PushConstants { uint64_t vertex; uint64_t pixel; uint64_t sharedAddress; };
[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;
#define g_point vk::RawBufferLoad<float4>(g_push.sharedAddress + 288)
#else
cbuffer SharedConstants : register(b2, space4) { float4 g_point : packoffset(c18); }
#endif

struct PointVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float4 fog : COLOR1;
    [[vk::location(2)]] float size : PSIZE0;
};

// DXIL links stages by declaration order, so this must match the mote pixel shader's input order.
struct SpriteVertex {
    float4 position : SV_Position;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float4 fog : COLOR1;
};

[maxvertexcount(4)]
void main(point PointVertex points[1], inout TriangleStream<SpriteVertex> stream) {
    PointVertex p = points[0];
    if (p.position.w <= 0.0) return;
    float diameter = min(max(p.size, g_point.z), g_point.w);
    float2 radius = diameter * g_point.xy * p.position.w;
#ifdef __spirv__
    radius.y = -radius.y;
#endif
    const float2 corners[4] = {float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1)};
    [unroll] for (uint i = 0; i < 4; ++i) {
        SpriteVertex v;
        v.position = p.position;
        v.position.xy += corners[i] * radius;
        v.color = p.color;
        v.fog = p.fog;
        v.uv = corners[i] * float2(0.5, -0.5) + 0.5;
        stream.Append(v);
    }
}
