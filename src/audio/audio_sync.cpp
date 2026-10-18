#include "audio/audio_sync.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace cut {

namespace {

using cd = std::complex<double>;

void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * std::numbers::pi / double(len) * (inverse ? 1 : -1);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& v : a) v /= double(n);
}

// Log level minus its 1 s moving average: what changes (speech, shots, clicks), not how loud
// each recording is overall.
std::vector<double> features(const std::vector<float>& env, double rate) {
    const size_t n = env.size();
    std::vector<double> e(n), out(n);
    for (size_t i = 0; i < n; ++i) e[i] = std::log(double(std::max(0.f, env[i])) + 1e-3);
    const size_t half = size_t(std::max(1.0, rate / 2));
    double sum = 0;
    size_t a = 0, b = 0;  // window [a, b)
    for (size_t i = 0; i < n; ++i) {
        const size_t wa = i > half ? i - half : 0, wb = std::min(n, i + half + 1);
        while (b < wb) sum += e[b++];
        while (a < wa) sum -= e[a++];
        out[i] = e[i] - sum / double(b - a);
    }
    return out;
}

} // namespace

SyncMatch matchEnvelopes(const std::vector<float>& xEnv, const std::vector<float>& yEnv, double rate) {
    SyncMatch m;
    if (xEnv.size() < 8 || yEnv.size() < 8 || rate <= 0) return m;
    const auto x = features(xEnv, rate), y = features(yEnv, rate);
    const long long nx = (long long)x.size(), ny = (long long)y.size();
    size_t n = 1;
    while (n < size_t(nx + ny)) n <<= 1;
    std::vector<cd> X(n), Y(n);
    for (long long i = 0; i < nx; ++i) X[size_t(i)] = x[size_t(i)];
    for (long long i = 0; i < ny; ++i) Y[size_t(i)] = y[size_t(i)];
    fft(X, false);
    fft(Y, false);
    for (size_t i = 0; i < n; ++i) X[i] *= std::conj(Y[i]);
    fft(X, true);  // X[k] = sum_i x[i + k] y[i] (circular)

    // lags with at least min(2 s, the shorter length) of overlap
    const long long minOverlap = std::min<long long>(std::min(nx, ny), (long long)(2 * rate));
    auto overlap = [&](long long L) { return std::min(nx, L + ny) - std::max(0LL, L); };
    auto value = [&](long long L) { return X[size_t((L % (long long)n + (long long)n) % (long long)n)].real(); };
    long long best = 0;
    double bestV = -1e300;
    for (long long L = -(ny - 1); L <= nx - 1; ++L) {
        if (overlap(L) < minOverlap) continue;
        const double v = value(L);
        if (v > bestV) bestV = v, best = L;
    }
    if (bestV <= 0) return m;
    const long long guard = (long long)(0.3 * rate);
    double second = 0;
    for (long long L = -(ny - 1); L <= nx - 1; ++L) {
        if (overlap(L) < minOverlap || std::llabs(L - best) <= guard) continue;
        second = std::max(second, value(L));
    }
    m.ok = true;
    m.lagSec = double(best) / rate;
    m.confidence = second > 0 ? bestV / second : 100.0;
    return m;
}

int refineLag(const std::vector<float>& x, const std::vector<float>& y, int lo, int hi) {
    const int nx = int(x.size()), ny = int(y.size());
    int best = lo;
    double bestV = -1;
    for (int L = lo; L <= hi; ++L) {
        double s = 0;
        const int i0 = std::max(0, -L), i1 = std::min(ny, nx - L);
        for (int i = i0; i < i1; ++i) s += double(x[size_t(i + L)]) * double(y[size_t(i)]);
        s = std::abs(s);
        if (s > bestV) bestV = s, best = L;
    }
    return best;
}

} // namespace cut
