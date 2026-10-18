#pragma once
// Integer time. One tick = 1/35,280,000 s, an exact multiple of every common frame
// duration (23.976 .. 60 fps, NTSC variants included) and of 44.1/48 kHz sample periods.
#include <cstdint>
#include <string>

namespace cut {

using Tick = int64_t;
constexpr Tick kTicksPerSecond = 35'280'000;
constexpr Tick kInvalidTick = INT64_MIN;

struct Rational {
    int64_t num = 0;
    int64_t den = 1;
    constexpr double toDouble() const { return den ? double(num) / double(den) : 0.0; }
    constexpr bool valid() const { return num > 0 && den > 0; }
    friend constexpr bool operator==(const Rational&, const Rational&) = default;
};

// 128-bit-safe a*b/c rounding toward negative infinity / nearest.
int64_t mulDivFloor(int64_t a, int64_t b, int64_t c);
int64_t mulDivRound(int64_t a, int64_t b, int64_t c);

inline Tick secondsToTicks(double s) { return Tick(s * double(kTicksPerSecond) + (s >= 0 ? 0.5 : -0.5)); }
inline double ticksToSeconds(Tick t) { return double(t) / double(kTicksPerSecond); }

// Duration of one frame at `fps` (exact for all standard rates).
Tick frameDuration(Rational fps);
// Frame index containing tick t (floor).
int64_t ticksToFrame(Tick t, Rational fps);
// Start tick of frame n.
Tick frameToTicks(int64_t frame, Rational fps);
// Snap t to the nearest frame boundary.
Tick snapToFrame(Tick t, Rational fps);
Tick floorToFrame(Tick t, Rational fps);

// Stream time base <-> ticks (FFmpeg boundary helpers; pure arithmetic).
Tick streamToTicks(int64_t pts, Rational timeBase);
int64_t ticksToStream(Tick t, Rational timeBase);

// Sample index at 48 kHz etc.
inline int64_t ticksToSamples(Tick t, int rate) { return mulDivFloor(t, rate, kTicksPerSecond); }
inline Tick samplesToTicks(int64_t s, int rate) { return mulDivFloor(s, kTicksPerSecond, rate); }

// HH:MM:SS:FF (non-drop-frame; NTSC rates count nominal frames).
std::string formatTimecode(Tick t, Rational fps);
// Parses HH:MM:SS:FF, MM:SS:FF, SS:FF, or a plain frame number. Returns false on error.
bool parseTimecode(const std::string& s, Rational fps, Tick& out);
// "1:23.45" style short duration for UI tables.
std::string formatDuration(Tick t);

// Nominal integer fps (e.g. 59.94 -> 60) used for timecode frame fields.
int nominalFps(Rational fps);

} // namespace cut
