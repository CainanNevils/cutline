#include <doctest/doctest.h>

#include "audio/audio_sync.h"

#include <cmath>
#include <random>
#include <vector>

using namespace cut;

namespace {

// "voice": bursts of filtered noise at random times over a quiet room tone (mono, 48 kHz)
std::vector<float> scene(double seconds, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    std::uniform_real_distribution<double> u(0, 1);
    const size_t n = size_t(48000 * seconds);
    std::vector<float> v(n);
    for (auto& s : v) s = 0.002f * g(rng);
    double t = 0.2;
    while (t < seconds - 1) {
        const double len = 0.08 + 0.5 * u(rng), amp = 0.1 + 0.5 * u(rng);
        float lp = 0;
        for (size_t i = size_t(t * 48000); i < size_t((t + len) * 48000) && i < n; ++i) {
            lp += 0.3f * (g(rng) - lp);
            v[i] += float(amp) * lp;
        }
        t += len + 0.05 + 0.6 * u(rng);
    }
    return v;
}

std::vector<float> envelope(const std::vector<float>& v, int hop) {
    std::vector<float> e(v.size() / size_t(hop));
    for (size_t i = 0; i < e.size(); ++i)
        for (int k = 0; k < hop; ++k) e[i] = std::max(e[i], std::abs(v[i * size_t(hop) + size_t(k)]));
    return e;
}

std::vector<float> decimate(const std::vector<float>& v, int f) {
    std::vector<float> o(v.size() / size_t(f));
    for (size_t i = 0; i < o.size(); ++i) {
        float s = 0;
        for (int k = 0; k < f; ++k) s += v[i * size_t(f) + size_t(k)];
        o[i] = s / float(f);
    }
    return o;
}

} // namespace

TEST_CASE("audio sync: a second recording of part of the scene is found to the sample") {
    const auto x = scene(90, 7);
    // y = x from 23.4567 s for 40 s, quieter, inverted polarity, with its own noise
    const double off = 23.4567;
    const size_t o = size_t(std::llround(off * 48000));
    std::mt19937 rng(99);
    std::normal_distribution<float> g(0.f, 1.f);
    std::vector<float> y(size_t(40 * 48000));
    for (size_t i = 0; i < y.size(); ++i) y[i] = -0.3f * x[o + i] + 0.003f * g(rng);

    const SyncMatch m = matchEnvelopes(envelope(x, 240), envelope(y, 240), 200.0);
    REQUIRE(m.ok);
    CHECK(std::abs(m.lagSec - off) <= 0.006);
    CHECK(m.confidence > 1.3);

    // sharpen at 8 kHz around the coarse lag
    const auto x8 = decimate(x, 6), y8 = decimate(y, 6);
    const int guess = int(std::lround(m.lagSec * 8000));
    const int lag = refineLag(x8, y8, guess - 80, guess + 80);
    CHECK(std::abs(lag / 8000.0 - off) <= 0.0002);
}

TEST_CASE("audio sync: the other way round (the second recording starts first)") {
    const auto base = scene(60, 3);
    // x starts 7.5 s into the scene, y covers 0..50 s: y(t) ~ x(t - 7.5) -> lag -7.5
    std::vector<float> x(base.begin() + 7 * 48000 + 24000, base.end());
    std::vector<float> y(base.begin(), base.begin() + 50 * 48000);
    const SyncMatch m = matchEnvelopes(envelope(x, 240), envelope(y, 240), 200.0);
    REQUIRE(m.ok);
    CHECK(std::abs(m.lagSec + 7.5) <= 0.006);
}

TEST_CASE("audio sync: unrelated recordings give a low confidence") {
    const SyncMatch m = matchEnvelopes(envelope(scene(40, 1), 240), envelope(scene(40, 2), 240), 200.0);
    CHECK(m.confidence < 1.3);
}
