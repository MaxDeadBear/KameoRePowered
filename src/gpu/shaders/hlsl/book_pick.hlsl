// The storybook page coordinate under each pixel of the open book, so the mouse pointer can be mapped onto the page texture exactly while the pages flutter.

// The input matches every translated vertex shader's output signature, which XenosRecomp fixes as position, sixteen texture coordinates and two colours.
struct PickInput {
    float4 position : SV_Position;
    float4 texcoord0 : TEXCOORD0;
    float4 texcoord1 : TEXCOORD1;
    float4 texcoord2 : TEXCOORD2;
    float4 texcoord3 : TEXCOORD3;
    float4 texcoord4 : TEXCOORD4;
    float4 texcoord5 : TEXCOORD5;
    float4 texcoord6 : TEXCOORD6;
    float4 texcoord7 : TEXCOORD7;
    float4 texcoord8 : TEXCOORD8;
    float4 texcoord9 : TEXCOORD9;
    float4 texcoord10 : TEXCOORD10;
    float4 texcoord11 : TEXCOORD11;
    float4 texcoord12 : TEXCOORD12;
    float4 texcoord13 : TEXCOORD13;
    float4 texcoord14 : TEXCOORD14;
    float4 texcoord15 : TEXCOORD15;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
};

// The book's page shader samples the page texture through TEXCOORD4; the third channel marks a covered pixel.
float4 PSMain(PickInput input) : SV_Target {
    return float4(input.texcoord4.xy, 1.0, 1.0);
}
