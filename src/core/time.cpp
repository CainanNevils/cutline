#include "core/time.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace cut {

namespace {
// Signed 128-bit helpers via MSVC intrinsics-free long arithmetic.
struct I128 {
    bool neg;
    unsigned long long hi, lo;
};

I128 mul64(int64_t a, int64_t b) {
    bool neg = (a < 0) != (b < 0);
    unsigned long long ua = a < 0 ? 0ull - (unsigned long long)a : (unsigned long long)a;
    unsigned long long ub = b < 0 ? 0ull - (unsigned long long)b : (unsigned long long)b;
    unsigned long long a0 = ua & 0xffffffffull, a1 = ua >> 32;
    unsigned long long b0 = ub & 0xffffffffull, b1 = ub >> 32;
    unsigned long long p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    unsigned long long mid = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);
    unsigned long long lo = (p00 & 0xffffffffull) | (mid << 32);
    unsigned long long hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return {neg && (hi | lo), hi, lo};
}

// Unsigned 128 / 64 by long division; returns quotient (assumed to fit 64 bits) and remainder.
unsigned long long div128(unsigned long long hi, unsigned long long lo, unsigned long long d,
                          unsigned long long& rem) {
    unsigned long long q = 0, r = hi % d;
    for (int i = 63; i >= 0; --i) {
        bool carry = (r >> 63) != 0;
        r = (r << 1) | ((lo >> i) & 1ull);
        q <<= 1;
        if (carry || r >= d) {
            r -= d;
            q |= 1ull;
        }
    }
    rem = r;
    return q;
}
} // namespace

int64_t mulDivFloor(int64_t a, int64_t b, int64_t c) {
    if (c < 0) { c = -c; b = -b; }
    I128 p = mul64(a, b);
    unsigned long long rem = 0;
    unsigned long long q = div128(p.hi, p.lo, (unsigned long long)c, rem);
    if (p.neg) {
        int64_t r = -(int64_t)q;
        if (rem) r -= 1;
        return r;
    }
    return (int64_t)q;
}

int64_t mulDivRound(int64_t a, int64_t b, int64_t c) {
    if (c < 0) { c = -c; b = -b; }
    I128 p = mul64(a, b);
    unsigned long long rem = 0;
    unsigned long long q = div128(p.hi, p.lo, (unsigned long long)c, rem);
    if (rem * 2 >= (unsigned long long)c) q += 1;  // round half away from zero
    return p.neg ? -(int64_t)q : (int64_t)q;
}

Tick frameDuration(Rational fps) {
    if (!fps.valid()) return kTicksPerSecond / 30;
    return mulDivRound(kTicksPerSecond, fps.den, fps.num);
}

int64_t ticksToFrame(Tick t, Rational fps) {
    if (!fps.valid()) return 0;
    // floor(t * num / (den * TPS)); then correct against frameToTicks' rounding so that
    // ticksToFrame(frameToTicks(n)) == n even for rates whose frame duration is not exact.
    int64_t f = mulDivFloor(t, fps.num, fps.den * kTicksPerSecond);
    if (frameToTicks(f + 1, fps) <= t) ++f;
    else if (frameToTicks(f, fps) > t) --f;
    return f;
}

Tick frameToTicks(int64_t frame, Rational fps) {
    if (!fps.valid()) return 0;
    return mulDivRound(frame, fps.den * kTicksPerSecond, fps.num);
}

Tick snapToFrame(Tick t, Rational fps) {
    if (!fps.valid()) return t;
    int64_t f = mulDivRound(t, fps.num, fps.den * kTicksPerSecond);
    return frameToTicks(f, fps);
}

Tick floorToFrame(Tick t, Rational fps) { return frameToTicks(ticksToFrame(t, fps), fps); }

Tick streamToTicks(int64_t pts, Rational tb) {
    if (!tb.valid()) return 0;
    return mulDivRound(pts, tb.num * kTicksPerSecond, tb.den);
}

int64_t ticksToStream(Tick t, Rational tb) {
    if (!tb.valid()) return 0;
    return mulDivRound(t, tb.den, tb.num * kTicksPerSecond);
}

int nominalFps(Rational fps) {
    if (!fps.valid()) return 30;
    return int(std::lround(fps.toDouble()));
}

std::string formatTimecode(Tick t, Rational fps) {
    bool neg = t < 0;
    if (neg) t = -t;
    int64_t frame = ticksToFrame(t, fps);
    int nf = nominalFps(fps);
    int64_t ff = frame % nf;
    int64_t totalSec = frame / nf;
    int64_t ss = totalSec % 60, mm = (totalSec / 60) % 60, hh = totalSec / 3600;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld:%02lld", neg ? "-" : "", (long long)hh,
                  (long long)mm, (long long)ss, (long long)ff);
    return buf;
}

bool parseTimecode(const std::string& sIn, Rational fps, Tick& out) {
    std::string s;
    for (char c : sIn)
        if (c != ' ') s += c;
    if (s.empty()) return false;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        s.erase(0, 1);
    }
    std::vector<long long> parts;
    std::string cur;
    for (char c : s) {
        if (c >= '0' && c <= '9') cur += c;
        else if (c == ':' || c == ';' || c == '.') {
            if (cur.empty()) return false;
            parts.push_back(std::stoll(cur));
            cur.clear();
        } else return false;
    }
    if (cur.empty()) return false;
    parts.push_back(std::stoll(cur));
    if (parts.size() > 4) return false;
    int nf = nominalFps(fps);
    long long frames = 0;
    if (parts.size() == 1 && s.size() <= 2) frames = parts[0];
    else if (parts.size() == 1) {
        // digits only, like Premiere: read right-aligned as HHMMSSFF ("500" = 5 s 00 f)
        if (s.size() > 8) return false;
        parts.clear();
        for (int end = int(s.size()); end > 0; end -= 2) {
            const int begin = std::max(0, end - 2);
            parts.insert(parts.begin(), std::stoll(s.substr(size_t(begin), size_t(end - begin))));
        }
    }
    if (parts.size() > 1) {
        long long ff = parts.back();
        long long secs = 0;
        for (size_t i = 0; i + 1 < parts.size(); ++i) secs = secs * 60 + parts[i];
        frames = secs * nf + ff;
    }
    out = frameToTicks(neg ? -frames : frames, fps);
    return true;
}

std::string formatDuration(Tick t) {
    double s = ticksToSeconds(t < 0 ? -t : t);
    int h = int(s / 3600);
    int m = int(s / 60) % 60;
    double sec = s - h * 3600 - m * 60;
    char buf[64];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%05.2f", h, m, sec);
    else std::snprintf(buf, sizeof(buf), "%d:%05.2f", m, sec);
    return buf;
}

} // namespace cut
