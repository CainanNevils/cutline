// Shared HLSL helpers. All compositing happens in the video's non-linear (gamma) RGB space with
// premultiplied alpha, like Premiere's default.

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

SamplerState gLinear : register(s0);  // linear, clamp
SamplerState gPoint : register(s1);   // point, clamp

// Fullscreen triangle (draw 3 vertices).
VSOut fullscreen(uint id) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float luma709(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
