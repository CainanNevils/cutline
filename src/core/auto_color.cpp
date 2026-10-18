#include "core/auto_color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cut {

namespace {
// Colour Correction white balance multipliers (linear light), as in layer.hlsl.
void wbGains(double T, double t, double g[3]) {
    g[0] = 1.0 + 0.25 * T - 0.05 * t;
    g[1] = 1.0 - 0.2 * t;
    g[2] = 1.0 - 0.25 * T - 0.05 * t;
}
double luma(double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
} // namespace

ColorFix autoColor(const uint8_t* rgba, int w, int h, int pitch) {
    ColorFix fix;
    if (!rgba || w <= 0 || h <= 0) return fix;
    static const std::array<double, 256> lin = [] {
        std::array<double, 256> a{};
        for (int i = 0; i < 256; ++i) a[size_t(i)] = std::pow(i / 255.0, 2.2);
        return a;
    }();
    // grey world on midtones, low-saturation pixels counting most
    double sum[3] = {0, 0, 0}, wsum = 0;
    size_t usable = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgba + size_t(y) * size_t(pitch);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            if (p[3] < 200) continue;
            ++usable;
            const double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
            const double Y = luma(r, g, b);
            if (Y < 0.12 || Y > 0.92) continue;
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
            const double sat = mx > 0 ? (mx - mn) / mx : 0;
            const double wgt = std::max(0.0, 1.0 - 1.6 * sat);
            sum[0] += wgt * lin[p[0]];
            sum[1] += wgt * lin[p[1]];
            sum[2] += wgt * lin[p[2]];
            wsum += wgt;
        }
    }
    if (usable < size_t(w) * size_t(h) / 10) return fix;
    double T = 0, t = 0;
    if (wsum > double(usable) * 0.02) {
        const double mR = sum[0] / wsum, mG = sum[1] / wsum, mB = sum[2] / wsum;
        // (gain_R) mR = (gain_G) mG and (gain_B) mB = (gain_G) mG, linear in T and t
        const double a11 = 0.25 * mR, a12 = 0.2 * mG - 0.05 * mR, b1 = mG - mR;
        const double a21 = -0.25 * mB, a22 = 0.2 * mG - 0.05 * mB, b2 = mG - mB;
        const double det = a11 * a22 - a12 * a21;
        if (std::abs(det) > 1e-9) {
            T = (b1 * a22 - a12 * b2) / det;
            t = (a11 * b2 - a21 * b1) / det;
            T = std::clamp(0.75 * T, -0.8, 0.8);
            t = std::clamp(0.75 * t, -0.8, 0.8);
        }
    }
    // tonal range after white balance: luma percentiles
    double g[3];
    wbGains(T, t, g);
    std::vector<uint32_t> hist(1024, 0);
    size_t n = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgba + size_t(y) * size_t(pitch);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            if (p[3] < 200) continue;
            const double r = std::pow(std::min(1.0, lin[p[0]] * g[0]), 1 / 2.2);
            const double gg = std::pow(std::min(1.0, lin[p[1]] * g[1]), 1 / 2.2);
            const double b = std::pow(std::min(1.0, lin[p[2]] * g[2]), 1 / 2.2);
            ++hist[size_t(std::clamp(luma(r, gg, b), 0.0, 1.0) * 1023.0)];
            ++n;
        }
    }
    auto pct = [&](double q) {
        const size_t target = size_t(q * double(n));
        size_t acc = 0;
        for (size_t i = 0; i < hist.size(); ++i)
            if ((acc += hist[i]) > target) return double(i) / 1023.0;
        return 1.0;
    };
    const double lo = pct(0.01), mid = pct(0.5), hi = pct(0.995);
    double e = 0;
    if (hi > 0.02 && hi < 0.9) e = std::clamp(2.2 * std::log2(0.93 / hi), 0.0, 1.5);  // dull: lift the top to near white
    else if (mid > 0.72) e = std::clamp(2.2 * std::log2(0.6 / mid), -1.5, 0.0);       // washed out
    const double scale = std::pow(2.0, e / 2.2);  // what exposure does to gamma values
    const double range = std::min(1.0, hi * scale) - lo * scale;
    double k = 1.0;
    if (range > 0.05 && range < 0.8) k = std::min(1.5, 0.88 / range);
    fix.exposure = std::round(e * 20.0) / 20.0;
    fix.contrast = std::round(std::clamp((k - 1.0) / 1.5 * 100.0, 0.0, 35.0));
    fix.temperature = std::round(T * 100.0);
    fix.tint = std::round(t * 100.0);
    fix.ok = true;
    return fix;
}

} // namespace cut
