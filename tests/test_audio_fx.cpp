#include <doctest/doctest.h>

#include "audio/audio_fx.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace cut;

namespace {

std::vector<float> tone(double dbfs, double hz, double seconds) {
    const int n = int(48000 * seconds);
    std::vector<float> v(size_t(n) * 2);
    const double a = std::pow(10.0, dbfs / 20.0);
    for (int i = 0; i < n; ++i) v[size_t(i) * 2] = v[size_t(i) * 2 + 1] = float(a * std::sin(2 * 3.14159265358979 * hz * i / 48000.0));
    return v;
}

// peak level (dBFS) of the last `seconds` (after filters / envelopes settled)
double tailPeakDb(const std::vector<float>& v, double seconds = 0.1) {
    const size_t from = v.size() - size_t(48000 * seconds) * 2;
    float pk = 0;
    for (size_t i = from; i < v.size(); ++i) pk = std::max(pk, std::abs(v[i]));
    return 20 * std::log10(std::max(1e-12f, pk));
}

Clip clipWith(Effect e) {
    Clip c;
    c.duration = kTicksPerSecond * 10;
    c.effects.push_back(std::move(e));
    return c;
}

double processedPeak(const Clip& c, double dbfs, double hz, double seconds = 0.5) {
    auto v = tone(dbfs, hz, seconds);
    ClipFxChain chain;
    chain.process(c, 0, v.data(), int(v.size() / 2));
    return tailPeakDb(v);
}

} // namespace

TEST_CASE("audio fx: high-pass / low-pass filters") {
    Effect hp = makeEffect(fx::HighPass);
    hp.params["frequency"].value = 1000;
    Clip c = clipWith(hp);
    CHECK(processedPeak(c, -6, 100) < -6 - 30);          // two octaves+ below: ~40 dB down
    CHECK(processedPeak(c, -6, 5003) == doctest::Approx(-6).epsilon(0.05));  // (not a divisor of 48 kHz: true peaks get sampled)

    Effect lp = makeEffect(fx::LowPass);
    lp.params["frequency"].value = 2000;
    Clip d = clipWith(lp);
    CHECK(processedPeak(d, -6, 10000) < -6 - 20);
    CHECK(processedPeak(d, -6, 200) == doctest::Approx(-6).epsilon(0.05));
}

TEST_CASE("audio fx: parametric EQ mid boost") {
    Effect eq = makeEffect(fx::Eq);
    eq.params["mid"].value = 6;
    eq.params["midFreq"].value = 1000;
    Clip c = clipWith(eq);
    CHECK(processedPeak(c, -20, 1000) == doctest::Approx(-14).epsilon(0.03));  // +6 dB at the centre
    CHECK(processedPeak(c, -20, 60) == doctest::Approx(-20).epsilon(0.05));    // far away: flat
}

TEST_CASE("audio fx: compressor reduces level above the threshold") {
    Effect comp = makeEffect(fx::Compressor);
    comp.params["threshold"].value = -20;
    comp.params["ratio"].value = 4;
    Clip c = clipWith(comp);
    // 12 dB over the threshold at 4:1 -> 3 dB over: about -17 dBFS
    CHECK(processedPeak(c, -8, 1000, 1.0) == doctest::Approx(-17).epsilon(0.08));
    // below the threshold: untouched
    CHECK(processedPeak(c, -30, 1000, 1.0) == doctest::Approx(-30).epsilon(0.02));
}

TEST_CASE("audio fx: noise gate closes on quiet input") {
    Effect gate = makeEffect(fx::NoiseGate);
    gate.params["threshold"].value = -40;
    gate.params["reduction"].value = -60;
    Clip c = clipWith(gate);
    CHECK(processedPeak(c, -55, 300, 1.0) < -100);                                 // hiss: gated
    CHECK(processedPeak(c, -20, 300, 1.0) == doctest::Approx(-20).epsilon(0.02));  // voice: open
}

TEST_CASE("audio fx: state carries across blocks") {
    Effect hp = makeEffect(fx::HighPass);
    hp.params["frequency"].value = 300;
    Effect comp = makeEffect(fx::Compressor);
    Clip c = clipWith(hp);
    c.effects.push_back(comp);
    auto whole = tone(-6, 440, 0.25), split = whole;
    ClipFxChain a, b;
    a.process(c, 0, whole.data(), int(whole.size() / 2));
    const int half = int(split.size() / 4);
    b.process(c, 0, split.data(), half);
    b.process(c, samplesToTicks(half, 48000), split.data() + size_t(half) * 2, int(split.size() / 2) - half);
    float worst = 0;
    for (size_t i = 0; i < whole.size(); ++i) worst = std::max(worst, std::abs(whole[i] - split[i]));
    CHECK(worst < 1e-6f);
}

TEST_CASE("audio fx: DeNoise lowers steady noise and keeps speech-level tones") {
    // white noise at about -40 dBFS throughout, a -20 dBFS tone from 2 s to 3 s
    const int n = 48000 * 4;
    std::vector<float> v(size_t(n) * 2);
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return (int(seed >> 9) & 0x3FFF) / 8192.0f - 1.0f;
    };
    for (int i = 0; i < n; ++i) {
        float x = 0.017f * rnd();
        if (i >= 2 * 48000 && i < 3 * 48000) x += 0.1f * float(std::sin(2 * 3.14159265358979 * 440 * i / 48000.0));
        v[size_t(i) * 2] = v[size_t(i) * 2 + 1] = x;
    }
    auto rmsDb = [](const std::vector<float>& s, int from, int to) {
        double e = 0;
        for (int i = from; i < to; ++i) e += double(s[size_t(i) * 2]) * s[size_t(i) * 2];
        return 10 * std::log10(e / (to - from) + 1e-20);
    };
    const double noiseBefore = rmsDb(v, 48000 + 12000, 2 * 48000 - 2000), toneBefore = rmsDb(v, 2 * 48000 + 9000, 3 * 48000 - 2000);
    Effect dn = makeEffect(fx::Denoise);
    dn.params["reduction"].value = 18;
    Clip c = clipWith(dn);
    REQUIRE(ClipFxChain::latency(c) == 512);
    ClipFxChain chain;
    for (int i = 0; i < n; i += 480) chain.process(c, 0, v.data() + size_t(i) * 2, std::min(480, n - i));
    const int L = 512;  // output lags the input by the latency
    const double noiseAfter = rmsDb(v, 48000 + 12000 + L, 2 * 48000 - 2000 + L);
    const double toneAfter = rmsDb(v, 2 * 48000 + 9000 + L, 3 * 48000 - 2000 + L);
    CHECK(noiseAfter < noiseBefore - 10);                     // steady noise goes down a lot
    CHECK(toneAfter == doctest::Approx(toneBefore).epsilon(0.08));  // the tone survives (within ~1.5 dB)
}
