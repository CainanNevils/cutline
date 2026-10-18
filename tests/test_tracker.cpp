#include <doctest/doctest.h>

#include "core/tracker.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace cut;

namespace {
constexpr int W = 320, H = 180;

// smooth random background, plus a 40 x 24 "name tag" (stripes and a gradient) centred at (x, y)
std::vector<uint8_t> frame(double x, double y, int brightness = 0) {
    std::vector<uint8_t> f(size_t(W) * H);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const double bg = 90 + 40 * std::sin(i * 0.11) * std::cos(j * 0.07) + 20 * std::sin((i + j) * 0.031);
            double v = bg;
            const double u = i - x, w = j - y;
            if (std::abs(u) < 20 && std::abs(w) < 12) v = ((int(u + 20) / 4) % 2 ? 230 : 40) - (w + 12) * 3;
            f[size_t(j) * W + i] = uint8_t(std::clamp(v + brightness, 0.0, 255.0));
        }
    return f;
}
} // namespace

TEST_CASE("region tracker follows a moving patch within a pixel or so") {
    RegionTracker t;
    auto pos = [](int k) { return std::pair<double, double>{60 + 3.3 * k, 90 + 25 * std::sin(k / 6.0)}; };
    auto [x0, y0] = pos(0);
    REQUIRE(t.start(frame(x0, y0).data(), W, H, x0, y0, 40, 24));
    double worst = 0;
    for (int k = 1; k <= 50; ++k) {
        auto [x, y] = pos(k);
        REQUIRE(t.track(frame(x, y, k % 7).data(), W, H));  // with flickering brightness
        worst = std::max(worst, std::hypot(t.x() - x, t.y() - y));
    }
    CHECK(worst < 1.5);  // (the test patch itself is drawn on whole pixels)
}

TEST_CASE("region tracker reports a lost region instead of jumping") {
    RegionTracker t;
    REQUIRE(t.start(frame(100, 90).data(), W, H, 100, 90, 40, 24));
    // the patch vanishes: a frame of plain background
    std::vector<uint8_t> empty = frame(-500, -500);
    const bool found = t.track(empty.data(), W, H);
    CHECK((!found || t.confidence() < 0.8));
    CHECK(std::abs(t.x() - 100) < 40);
}

TEST_CASE("best window holds the most energy, the middle when it is all the same") {
    std::vector<double> e(100, 0.0);
    for (int i = 70; i < 80; ++i) e[size_t(i)] = 1;
    const int at = bestWindow(e, 20);
    CHECK(at <= 70);
    CHECK(at + 20 >= 80);
    CHECK(bestWindow(std::vector<double>(100, 1.0), 20) == 40);
    CHECK(bestWindow(e, 200) == 0);
}

TEST_CASE("region tracker refuses a flat area") {
    std::vector<uint8_t> flat(size_t(W) * H, 128);
    RegionTracker t;
    CHECK_FALSE(t.start(flat.data(), W, H, 100, 90, 40, 24));
}
