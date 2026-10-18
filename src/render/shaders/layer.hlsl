// One draw per visible clip: source decode format -> RGB, flip, crop, blur / sharpen, colour
// correction, LUT, black & white, vignette, opacity, onto the canvas through a 2D affine quad.
#include "common.hlsli"

cbuffer LayerCB : register(b0) {
    float4 corners[4];  // xy = clip-space position, zw = source uv (TL, TR, BL, BR)
    float4 cropRect;    // visible source uv region u0 v0 u1 v1
    float4 yuvRow0;     // rgb = M * (yuv - offset)
    float4 yuvRow1;
    float4 yuvRow2;
    float4 yuvOffset;
    int srcMode;   // 0 = planar YUV, 1 = RGBA straight alpha, 2 = RGBA premultiplied
    int transfer;  // 0 = SDR, 1 = PQ, 2 = HLG (display as SDR with a simple tone map)
    float opacity;
    float blend2;  // frame blend weight of the second source (0 = off)
    float4 cc0;    // exposure (stops), contrast (-1..1), highlights (-1..1), shadows (-1..1)
    float4 cc1;    // saturation (0..2), temperature (-1..1), tint (-1..1), enabled
    float4 lutP;   // intensity (0..1), size, enabled, lut-before-cc
    float4 fx0;    // flip h, flip v, black & white (0..1), sharpen (0..2)
    float4 fx1;    // vignette amount (-1..1), midpoint, feather
    float4 fx2;    // blur radius in uv (x, y), texel size in uv (x, y)
    float4 fx3;    // visible uv extent of the texture (padded NV12 < 1), corner radius, border width (px)
    float4 fx4;    // border colour, shadow opacity
    float4 fx5;    // shadow offset (px), softness (px)
    float4 fx6;    // chroma key: colour (0 off, 1 green, 2 blue), tolerance, softness, spill
    float4 fx7;    // mask centre, size (fractions of the image)
    float4 fx8;    // mask shape (0 off, 1 ellipse, 2 rectangle), feather px, invert, mosaic block (px, 0 off)
    float4 fx9;    // glow intensity (0 off), threshold, radius in uv (x, y)
};

Texture2D tPlane0 : register(t0);
Texture2D tPlane1 : register(t1);
Texture2D tPlane0b : register(t2);
Texture2D tPlane1b : register(t3);
Texture3D tLut : register(t4);

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float4 c = corners[id];
    o.pos = float4(c.xy, 0, 1);
    o.uv = c.zw;
    return o;
}

float3 pqToLinear(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0) / (c2 - c3 * p), 1.0 / m1) * 10000.0 / 203.0;  // 203 nits = SDR white
}
float3 hlgToLinear(float3 e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 lo = e * e / 3.0;
    float3 hi = (exp((e - c) / a) + b) / 12.0;
    return (e <= 0.5 ? lo : hi) * 4.0;
}
float3 toneMapToSdr(float3 rgb2020, int tf) {
    float3 lin = tf == 1 ? pqToLinear(rgb2020) : hlgToLinear(rgb2020);
    // BT.2020 -> BT.709 primaries
    float3 l709 = float3(dot(lin, float3(1.6605, -0.5876, -0.0728)), dot(lin, float3(-0.1246, 1.1329, -0.0083)),
                         dot(lin, float3(-0.0182, -0.1006, 1.1187)));
    l709 = max(l709, 0);
    l709 = l709 / (1.0 + l709 * 0.25);  // gentle Reinhard
    return pow(saturate(l709), 1.0 / 2.4);
}

// SampleLevel: no mips, and safe inside the blur loop (no derivatives needed)
float4 sampleSource(Texture2D p0, Texture2D p1, float2 uv) {
    float4 c;
    if (srcMode == 0) {
        float y = p0.SampleLevel(gLinear, uv, 0).r;
        float2 cbcr = p1.SampleLevel(gLinear, uv, 0).rg;
        float3 yuv = float3(y, cbcr) - yuvOffset.xyz;
        float3 rgb = float3(dot(yuvRow0.xyz, yuv), dot(yuvRow1.xyz, yuv), dot(yuvRow2.xyz, yuv));
        if (transfer != 0) rgb = toneMapToSdr(saturate(rgb), transfer);
        c = float4(saturate(rgb), 1);
    } else {
        c = p0.SampleLevel(gLinear, uv, 0);
        if (srcMode == 1) c.rgb *= c.a;
    }
    return c;
}

float3 colorCorrect(float3 c) {
    // exposure in (approximately) linear light
    float3 lin = pow(max(c, 0), 2.2) * exp2(cc0.x);
    // white balance
    lin *= float3(1.0 + 0.25 * cc1.y - 0.05 * cc1.z, 1.0 - 0.2 * cc1.z, 1.0 - 0.25 * cc1.y - 0.05 * cc1.z);
    c = pow(max(lin, 0), 1.0 / 2.2);
    // highlights / shadows: luminance-weighted lift
    float L = luma709(saturate(c));
    float ws = 1.0 - smoothstep(0.0, 0.55, L);
    float wh = smoothstep(0.45, 1.0, L);
    c += cc0.w * 0.35 * ws + cc0.z * 0.35 * wh;
    // contrast around mid grey
    float k = cc0.y >= 0 ? 1.0 + cc0.y * 1.5 : 1.0 + cc0.y;
    c = (c - 0.5) * k + 0.5;
    // saturation
    float l2 = luma709(c);
    c = lerp(l2.xxx, c, cc1.x);
    return saturate(c);
}

float3 applyLut(float3 rgb) {
    float n = lutP.y;
    float3 lc = saturate(rgb) * ((n - 1) / n) + 0.5 / n;
    return lerp(rgb, tLut.SampleLevel(gLinear, lc, 0).rgb, lutP.x);
}

// Gaussian blur: Vogel (golden-angle) disk, gaussian weights; or a 5-tap unsharp mask.
float4 sampleFiltered(Texture2D p0, Texture2D p1, float2 uv) {
    float4 c;
    if (fx2.x > 0) {
        const int N = 48;
        float4 acc = 0;
        float wsum = 0;
        [loop] for (int k = 0; k < N; ++k) {
            float r = sqrt((k + 0.5) / N);
            float th = k * 2.39996323;
            float w = exp(-r * r * 2.5);
            acc += sampleSource(p0, p1, uv + float2(cos(th), sin(th)) * r * fx2.xy) * w;
            wsum += w;
        }
        c = acc / wsum;
    } else {
        c = sampleSource(p0, p1, uv);
        if (fx0.w > 0) {
            float4 n = sampleSource(p0, p1, uv + float2(fx2.z, 0)) + sampleSource(p0, p1, uv - float2(fx2.z, 0)) +
                       sampleSource(p0, p1, uv + float2(0, fx2.w)) + sampleSource(p0, p1, uv - float2(0, fx2.w));
            c = saturate(c + (c - n * 0.25) * fx0.w * 1.5);
            c.rgb = min(c.rgb, c.a);  // stay premultiplied
        }
    }
    return c;
}

// Glow: blurred bright parts (above the threshold) added back onto the picture.
float3 glowAt(Texture2D p0, Texture2D p1, float2 uv) {
    const int N = 32;
    float3 acc = 0;
    float wsum = 0;
    [loop] for (int k = 0; k < N; ++k) {
        float r = sqrt((k + 0.5) / N);
        float th = k * 2.39996323;
        float w = exp(-r * r * 2.0);
        float4 s = sampleSource(p0, p1, uv + float2(cos(th), sin(th)) * r * fx9.zw);
        float3 rgb = s.a > 0 ? s.rgb / s.a : 0;
        float l = luma709(rgb);
        acc += rgb * saturate((l - fx9.y) / max(1 - fx9.y, 1e-3)) * s.a * w;
        wsum += w;
    }
    return acc / wsum * fx9.x;
}

float4 ps_main(VSOut i) : SV_Target {
    float2 uv = i.uv;
    if (fx0.x > 0) uv.x = fx3.x - uv.x;
    if (fx0.y > 0) uv.y = fx3.y - uv.y;
    // drop shadow: soft (rounded) rectangle behind the image, offset in source pixels
    float shadowA = 0;
    if (fx4.w > 0) {
        float2 lo = cropRect.xy / fx2.zw, size = (cropRect.zw - cropRect.xy) / fx2.zw;
        float2 px = uv / fx2.zw - lo - fx5.xy;
        float r = min(fx3.z, 0.5 * min(size.x, size.y));
        float2 q = abs(px - size * 0.5) - (size * 0.5 - r);
        float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
        float s = max(fx5.z, 1.0);
        shadowA = fx4.w * (1 - smoothstep(-s * 0.5, s * 0.5, d));
    }
    if (uv.x < cropRect.x || uv.y < cropRect.y || uv.x > cropRect.z || uv.y > cropRect.w) return float4(0, 0, 0, shadowA) * opacity;
    // mosaic: every pixel of a block shows the block's centre (the mask below keeps sharp edges)
    float2 suv = uv;
    if (fx8.w > 0) {
        float2 b = fx8.w * fx2.zw;
        suv = clamp((floor(uv / b) + 0.5) * b, cropRect.xy, cropRect.zw);
    }
    float4 c = sampleFiltered(tPlane0, tPlane1, suv);
    if (blend2 > 0) c = lerp(c, sampleFiltered(tPlane0b, tPlane1b, suv), blend2);
    if (fx9.x > 0) c.rgb = min(c.rgb + glowAt(tPlane0, tPlane1, suv), max(c.a, 1e-4));  // (stays premultiplied)
    if (cc1.w > 0 || lutP.z > 0 || fx0.z > 0 || fx1.x != 0 || fx6.x > 0) {
        float a = c.a;
        float3 rgb = a > 0 ? c.rgb / a : 0;
        if (fx6.x > 0) {
            // key on chroma direction: how far the colour leans toward the key hue beyond its
            // sideways deviation (greens of any brightness key; greys and skin do not)
            float3 kc = fx6.x < 1.5 ? float3(0, 1, 0) : float3(0, 0, 1);
            float ky = luma709(kc), py = luma709(rgb);
            float2 kcc = float2((kc.b - ky) / 1.8556, (kc.r - ky) / 1.5748);
            float2 pcc = float2((rgb.b - py) / 1.8556, (rgb.r - py) / 1.5748);
            float L = length(kcc);
            float2 kdir = kcc / L;
            float proj = dot(pcc, kdir);
            float perp = length(pcc - proj * kdir);
            float g = (proj - perp) / L;
            float lo = 0.4 * (1 - fx6.y), hi = lo + 0.02 + 0.5 * fx6.z;
            a *= 1 - smoothstep(lo, hi, g);
            if (fx6.x < 1.5) rgb.g = lerp(rgb.g, min(rgb.g, max(rgb.r, rgb.b)), fx6.w);
            else rgb.b = lerp(rgb.b, min(rgb.b, max(rgb.r, rgb.g)), fx6.w);
        }
        if (lutP.z > 0 && lutP.w > 0) rgb = applyLut(rgb);
        if (cc1.w > 0) rgb = colorCorrect(rgb);
        if (lutP.z > 0 && lutP.w <= 0) rgb = applyLut(rgb);
        if (fx0.z > 0) rgb = lerp(rgb, luma709(saturate(rgb)).xxx, fx0.z);
        if (fx1.x != 0) {
            // distance from the image centre: 0 at the centre, 1 at the corners
            float2 p = (i.uv / max(fx3.xy, 1e-4) - 0.5) * 2.0;
            float d = length(p) * 0.70710678;
            float v = smoothstep(fx1.y - fx1.z * 0.5, fx1.y + fx1.z * 0.5, d);
            rgb = fx1.x > 0 ? rgb * (1.0 - fx1.x * v) : lerp(rgb, 1.0, -fx1.x * v);
        }
        c = float4(saturate(rgb) * a, a);
    }
    if (fx8.x > 0) {
        // shape mask in visible image pixels
        float2 vis = fx3.xy / fx2.zw;
        float2 p = uv / fx3.xy * vis, cpx = fx7.xy * vis, hs = max(0.5 * fx7.zw * vis, 1e-3);
        float d;
        if (fx8.x < 1.5) d = (length((p - cpx) / hs) - 1) * min(hs.x, hs.y);
        else {
            float2 q = abs(p - cpx) - hs;
            d = length(max(q, 0)) + min(max(q.x, q.y), 0);
        }
        float f = max(fx8.y, 1.0);
        float m = 1 - smoothstep(-f * 0.5, f * 0.5, d);
        if (fx8.z > 0) m = 1 - m;
        c *= m;
    }
    if (fx3.z > 0 || fx3.w > 0) {
        // rounded rectangle (signed distance in source pixels) over the cropped image
        float2 lo = cropRect.xy / fx2.zw, hi = cropRect.zw / fx2.zw;
        float2 px = uv / fx2.zw - lo, size = hi - lo;
        float r = min(fx3.z, 0.5 * min(size.x, size.y));
        float2 q = abs(px - size * 0.5) - (size * 0.5 - r);
        float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
        float aa = max(0.75, 0.75 * length(fwidth(px)));
        float inside = 1 - smoothstep(-aa, aa, d);
        if (fx3.w > 0) {
            float b = smoothstep(-fx3.w - aa, -fx3.w + aa, d);  // 1 within the border band
            c = lerp(c, float4(fx4.rgb, 1), b);
        }
        c *= inside;
    }
    c += float4(0, 0, 0, shadowA) * (1 - c.a);  // image over its shadow
    return c * opacity;
}
