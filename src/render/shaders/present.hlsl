// Present / blit / overlay / colour conversion passes.
#include "common.hlsli"

cbuffer BlitCB : register(b0) {
    float4 srcRect;   // uv rect u0 v0 u1 v1
    float4 color;     // overlay colour (premultiplied) / background
    float4 dstRect;   // overlay rect in clip space x0 y0 x1 y1
    float4 params;    // x = matrix (0 709, 1 601), y = 1/width, z = 1/height, w = opaque flag
};

Texture2D tSrc : register(t0);

VSOut vs_blit(uint id : SV_VertexID) {
    VSOut o = fullscreen(id);
    o.uv = lerp(srcRect.xy, srcRect.zw, o.uv);
    return o;
}

// canvas (premultiplied) over a solid background
float4 ps_blit(VSOut i) : SV_Target {
    float4 c = tSrc.Sample(gLinear, i.uv);
    float4 o = c + color * (1 - c.a);
    if (params.w > 0) o.a = 1;
    return o;
}

cbuffer QuadCB : register(b1) {
    float4 quad[4];  // clip-space corners TL, TR, BL, BR (xy)
};

VSOut vs_rect(uint id : SV_VertexID) {
    VSOut o;
    o.pos = float4(quad[id].xy, 0, 1);
    o.uv = float2(id & 1, id >> 1);
    return o;
}

float4 ps_rect(VSOut i) : SV_Target { return color; }

// --- RGB (premultiplied over black) -> NV12, BT.709 or BT.601 limited range -------------------
float3 coeffs() { return params.x > 0.5 ? float3(0.299, 0.587, 0.114) : float3(0.2126, 0.7152, 0.0722); }

float ps_luma(VSOut i) : SV_Target {
    float3 rgb = saturate(tSrc.Sample(gPoint, i.uv).rgb);
    float y = dot(rgb, coeffs());
    return 16.0 / 255.0 + y * (219.0 / 255.0);
}

float2 ps_chroma(VSOut i) : SV_Target {
    // i.uv is the centre of a 2x2 block: bilinear sampling averages the four pixels.
    float3 rgb = saturate(tSrc.Sample(gLinear, i.uv).rgb);
    float3 k = coeffs();
    float y = dot(rgb, k);
    float cb = (rgb.b - y) / (2.0 * (1.0 - k.z));
    float cr = (rgb.r - y) / (2.0 * (1.0 - k.x));
    return float2(128.0 / 255.0 + cb * (224.0 / 255.0), 128.0 / 255.0 + cr * (224.0 / 255.0));
}
