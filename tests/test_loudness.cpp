#include <doctest/doctest.h>

#include "audio/loudness.h"

#include <cmath>
#include <vector>

using namespace cut;

namespace {
std::vector<float> sine(double dbfs, double hz, double seconds, bool stereo = true) {
    const int n = int(48000 * seconds);
    std::vector<float> v(size_t(n) * 2);
    const double a = std::pow(10.0, dbfs / 20.0);
    for (int i = 0; i < n; ++i) {
        float s = float(a * std::sin(2 * 3.14159265358979 * hz * i / 48000.0));
        v[size_t(i) * 2] = s;
        v[size_t(i) * 2 + 1] = stereo ? s : 0.f;
    }
    return v;
}
} // namespace

TEST_CASE("BS.1770 integrated loudness matches EBU Tech 3341 references") {
    // Case 1: stereo 1 kHz sine at -23 dBFS -> -23.0 LUFS (+-0.1)
    LoudnessMeter m;
    auto s = sine(-23, 1000, 20);
    m.push(s.data(), int(s.size() / 2));
    CHECK(m.integrated() == doctest::Approx(-23.0).epsilon(0.005));
    // Case 2: -33 dBFS -> -33.0 LUFS
    LoudnessMeter m2;
    auto s2 = sine(-33, 1000, 20);
    m2.push(s2.data(), int(s2.size() / 2));
    CHECK(m2.integrated() == doctest::Approx(-33.0).epsilon(0.005));
    // Case 3 (gating): -36 dBFS 10 s, -23 dBFS 60 s, -36 dBFS 10 s -> -23.0 LUFS
    LoudnessMeter m3;
    for (auto [db, sec] : {std::pair{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}}) {
        auto x = sine(db, 1000, sec);
        m3.push(x.data(), int(x.size() / 2));
    }
    CHECK(m3.integrated() == doctest::Approx(-23.0).epsilon(0.005));
    // silence is -inf, not a number that would produce a huge gain
    LoudnessMeter m4;
    std::vector<float> z(48000 * 2 * 3, 0.f);
    m4.push(z.data(), 48000 * 3);
    CHECK(std::isinf(m4.integrated()));
}

TEST_CASE("momentary and short-term loudness follow the latest audio") {
    LoudnessMeter m;
    auto loud = sine(-23, 1000, 4.0);
    m.push(loud.data(), int(loud.size() / 2));
    CHECK(m.momentary() == doctest::Approx(-23.0).epsilon(0.01));
    CHECK(m.shortTerm() == doctest::Approx(-23.0).epsilon(0.01));
    auto quiet = sine(-33, 1000, 0.5);
    m.push(quiet.data(), int(quiet.size() / 2));
    CHECK(m.momentary() == doctest::Approx(-33.0).epsilon(0.01));  // 400 ms window: only the new level
    CHECK(m.shortTerm() > -26.0);                                   // 3 s window still mostly the loud part
    m.reset();
    CHECK(m.shortTerm() < -200);
}
