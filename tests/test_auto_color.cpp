#include <doctest/doctest.h>

#include "core/auto_color.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace cut;

namespace {

// a "scene": grey-ish surfaces of varied brightness with some coloured objects, then a cast
// (linear gains) and a tonal squeeze applied
std::vector<uint8_t> scene(double castR, double castB, double top, unsigned seed) {
    const int w = 160, h = 90;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double v = 0.1 + 0.85 * u(rng);  // grey value (gamma)
            double r = v, g = v, b = v;
            if (u(rng) < 0.15) {  // a coloured object
                r = u(rng), g = u(rng), b = u(rng);
            }
            auto enc = [&](double c, double gain) {
                const double l = std::pow(c, 2.2) * gain;
                return uint8_t(std::lround(std::clamp(std::pow(std::min(1.0, l), 1 / 2.2) * top, 0.0, 1.0) * 255));
            };
            uint8_t* p = &px[(size_t(y) * w + size_t(x)) * 4];
            p[0] = enc(r, castR);
            p[1] = enc(g, 1.0);
            p[2] = enc(b, castB);
            p[3] = 255;
        }
    return px;
}

// mean of the grey pixels' channels after the fix's white balance (linear)
void corrected(const ColorFix& f, double castR, double castB, double out[3]) {
    const double T = f.temperature / 100.0, t = f.tint / 100.0;
    out[0] = castR * (1 + 0.25 * T - 0.05 * t);
    out[1] = 1.0 * (1 - 0.2 * t);
    out[2] = castB * (1 - 0.25 * T - 0.05 * t);
}

} // namespace

TEST_CASE("auto color: a neutral, full-range frame is left alone") {
    auto px = scene(1.0, 1.0, 1.0, 1);
    const ColorFix f = autoColor(px.data(), 160, 90, 160 * 4);
    REQUIRE(f.ok);
    CHECK(std::abs(f.temperature) <= 3);
    CHECK(std::abs(f.tint) <= 3);
    CHECK(std::abs(f.exposure) <= 0.1);
    CHECK(f.contrast <= 3);
}

TEST_CASE("auto color: a warm cast is mostly removed") {
    const double castR = 1.35, castB = 0.75;  // orange light
    auto px = scene(castR, castB, 1.0, 2);
    const ColorFix f = autoColor(px.data(), 160, 90, 160 * 4);
    REQUIRE(f.ok);
    CHECK(f.temperature < -20);  // cooler
    double c[3];
    corrected(f, castR, castB, c);
    // the R/B imbalance shrinks to well under half
    CHECK(std::abs(c[0] / c[2] - 1.0) < 0.5 * std::abs(castR / castB - 1.0));
}

TEST_CASE("auto color: a dull frame gets brighter and punchier") {
    auto px = scene(1.0, 1.0, 0.6, 3);  // nothing above 60 %
    const ColorFix f = autoColor(px.data(), 160, 90, 160 * 4);
    REQUIRE(f.ok);
    CHECK(f.exposure > 0.5);
}

TEST_CASE("auto color: transparent frames are not analysed") {
    std::vector<uint8_t> px(size_t(64) * 64 * 4, 0);
    CHECK_FALSE(autoColor(px.data(), 64, 64, 64 * 4).ok);
}
