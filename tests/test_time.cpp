#include <doctest/doctest.h>

#include "core/time.h"

using namespace cut;

TEST_CASE("tick base is an exact multiple of common frame and sample periods") {
    const Rational rates[] = {{24, 1}, {25, 1}, {30, 1}, {50, 1}, {60, 1}, {24000, 1001}, {30000, 1001}, {60000, 1001},
                              {120, 1}, {48, 1}};
    for (Rational r : rates) {
        INFO(r.num << "/" << r.den);
        CHECK((kTicksPerSecond * r.den) % r.num == 0);
    }
    CHECK(kTicksPerSecond % 44100 == 0);
    CHECK(kTicksPerSecond % 48000 == 0);
    CHECK(frameDuration({60, 1}) == 588000);
    CHECK(frameDuration({60000, 1001}) == 588588);
    CHECK(frameDuration({24000, 1001}) == 1471470);
}

TEST_CASE("frame <-> tick round trips") {
    const Rational rates[] = {{24, 1}, {30, 1}, {60, 1}, {24000, 1001}, {30000, 1001}, {60000, 1001}, {144, 1}};
    for (Rational r : rates)
        for (int64_t n : {0LL, 1LL, 2LL, 59LL, 60LL, 61LL, 1799LL, 215999LL, 10000000LL}) {
            Tick t = frameToTicks(n, r);
            CHECK(ticksToFrame(t, r) == n);
            CHECK(ticksToFrame(t + frameDuration(r) - 1, r) == n);
            CHECK(snapToFrame(t + 3, r) == t);
        }
    CHECK(ticksToFrame(-1, {60, 1}) == -1);
}

TEST_CASE("timecode") {
    Rational fps{60, 1};
    Tick t = frameToTicks(((1 * 60 + 2) * 60 + 3) * 60 + 4, fps);
    CHECK(formatTimecode(t, fps) == "01:02:03:04");
    Tick back = 0;
    REQUIRE(parseTimecode("01:02:03:04", fps, back));
    CHECK(back == t);
    REQUIRE(parseTimecode("3:04", fps, back));
    CHECK(back == frameToTicks(3 * 60 + 4, fps));
    REQUIRE(parseTimecode("12", fps, back));
    CHECK(back == frameToTicks(12, fps));
    // digits only: right-aligned HHMMSSFF like Premiere ("120" = 1 s 20 f, "10000" = 1 min)
    REQUIRE(parseTimecode("120", fps, back));
    CHECK(back == frameToTicks(60 + 20, fps));
    REQUIRE(parseTimecode("10000", fps, back));
    CHECK(back == frameToTicks(60 * 60, fps));
    REQUIRE(parseTimecode("-500", fps, back));
    CHECK(back == -frameToTicks(5 * 60, fps));
    CHECK_FALSE(parseTimecode("ab:cd", fps, back));
    CHECK(formatTimecode(frameToTicks(59, {60000, 1001}), {60000, 1001}) == "00:00:00:59");
}

TEST_CASE("mulDiv handles large values") {
    CHECK(mulDivFloor(INT64_C(1) << 40, INT64_C(1) << 30, INT64_C(1) << 35) == (INT64_C(1) << 35));
    CHECK(mulDivFloor(-7, 1, 2) == -4);
    CHECK(mulDivRound(7, 1, 2) == 4);
    CHECK(mulDivRound(-7, 1, 2) == -4);
    // 10 hours of ticks at 60000/1001 does not overflow
    Tick t10h = 10 * 3600 * kTicksPerSecond;
    CHECK(ticksToFrame(t10h, {60000, 1001}) == 2157842);
    CHECK(streamToTicks(1001, {1, 60000}) == 588588);
    CHECK(ticksToStream(588588, {1, 60000}) == 1001);
}
