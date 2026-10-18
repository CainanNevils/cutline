// Combines two full-frame premultiplied layers (outgoing A, incoming B) at a progress 0..1.
#include "common.hlsli"

cbuffer TransitionCB : register(b0) {
    int type;  // matches cut::TransitionType
    float progress;
    float2 texel;  // 1/width, 1/height
};

Texture2D tA : register(t0);
Texture2D tB : register(t1);

VSOut vs_main(uint id : SV_VertexID) { return fullscreen(id); }

float4 over(float4 top, float4 bottom) { return top + bottom * (1 - top.a); }

float4 ps_main(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float p = saturate(progress);
    float4 a = tA.SampleLevel(gPoint, uv, 0);
    float4 b = tB.SampleLevel(gPoint, uv, 0);
    const float feather = 0.004;
    if (type == 0) return lerp(a, b, p);  // cross dissolve
    if (type == 1 || type == 2) {         // dip to black / white
        float4 dip = type == 1 ? float4(0, 0, 0, 1) : float4(1, 1, 1, 1);
        return p < 0.5 ? lerp(a, dip, p * 2) : lerp(dip, b, p * 2 - 1);
    }
    if (type >= 3 && type <= 6) {  // wipes: edge travels in the named direction
        float m;
        if (type == 3) m = smoothstep(1 - p - feather, 1 - p + feather, uv.x);       // left
        else if (type == 4) m = 1 - smoothstep(p - feather, p + feather, uv.x);      // right
        else if (type == 5) m = smoothstep(1 - p - feather, 1 - p + feather, uv.y);  // up
        else m = 1 - smoothstep(p - feather, p + feather, uv.y);                     // down
        if (p >= 1) m = 1;
        if (p <= 0) m = 0;
        return lerp(a, b, m);
    }
    if (type == 7) {  // slide: B pushes in from the right over A
        float2 ub = uv - float2(1 - p, 0);
        float4 bs = (ub.x >= 0 && ub.x <= 1) ? tB.SampleLevel(gPoint, ub, 0) : 0;
        return over(bs, a);
    }
    if (type == 9) {  // push: B enters from the right and pushes A out to the left (eased)
        float e = smoothstep(0, 1, p);
        float2 ua = uv + float2(e, 0), ub = uv - float2(1 - e, 0);
        if (ub.x >= 0) return tB.SampleLevel(gLinear, ub, 0);
        return tA.SampleLevel(gLinear, ua, 0);
    }
    if (type == 10) {  // cross zoom: A zooms in while fading to B, which settles from zoomed in
        float za = 1 + 1.5 * p * p, zb = 1 + 1.5 * (1 - p) * (1 - p);
        float4 ca = tA.SampleLevel(gLinear, (uv - 0.5) / za + 0.5, 0);
        float4 cb = tB.SampleLevel(gLinear, (uv - 0.5) / zb + 0.5, 0);
        return lerp(ca, cb, smoothstep(0.25, 0.75, p));
    }
    if (type == 11) {  // iris round: a growing circle (aspect-correct) reveals B
        float2 d = (uv - 0.5) * float2(texel.y / texel.x, 1);
        float r = length(d), rmax = length(float2(0.5 * texel.y / texel.x, 0.5));
        float edge = p * rmax * 1.02;
        float m = 1 - smoothstep(edge - 0.004, edge + 0.004, r);
        if (p >= 1) m = 1;
        return lerp(a, b, m);
    }
    if (type == 12) {  // blur dissolve: both blur toward the middle while crossfading
        float radius = 40 * (1 - abs(p * 2 - 1));
        float4 sa = 0, sb = 0;
        float wsum = 0;
        [loop] for (int k = 0; k < 24; ++k) {
            float rr = sqrt((k + 0.5) / 24.0);
            float th = k * 2.39996323;
            float2 o = float2(cos(th), sin(th)) * rr * radius * texel;
            float w = exp(-rr * rr * 2.5);
            sa += tA.SampleLevel(gLinear, uv + o, 0) * w;
            sb += tB.SampleLevel(gLinear, uv + o, 0) * w;
            wsum += w;
        }
        return lerp(sa / wsum, sb / wsum, p);
    }
    if (type == 13) {  // whip: a fast push with horizontal motion blur, strongest in the middle
        float e = smoothstep(0, 1, p);
        float blur = 0.08 * sin(p * 3.14159265);  // blur length in uv
        float4 acc = 0;
        [loop] for (int k = 0; k < 16; ++k) {
            float o = (k / 15.0 - 0.5) * blur;
            float2 ua = uv + float2(e + o, 0), ub = uv - float2(1 - e - o, 0);
            acc += ub.x >= 0 ? tB.SampleLevel(gLinear, ub, 0) : tA.SampleLevel(gLinear, ua, 0);
        }
        return acc / 16;
    }
    return lerp(a, b, p);
}
