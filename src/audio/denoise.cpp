#include "audio/denoise.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cut {

namespace {
constexpr int N = Denoiser::kSize;
constexpr int H = Denoiser::kHop;
constexpr int Bins = N / 2 + 1;
} // namespace

Denoiser::Denoiser() {
    // sqrt-Hann analysis and synthesis windows: their product (Hann) overlap-adds to a constant
    // (2 at 75 % overlap, compensated in frame())
    window_.resize(N);
    for (int i = 0; i < N; ++i) window_[size_t(i)] = float(std::sqrt(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / N)));
    bitrev_.resize(N);
    int bits = 0;
    while ((1 << bits) < N) ++bits;
    for (int i = 0; i < N; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev_[size_t(i)] = r;
    }
    twiddle_.resize(N / 2);
    for (int k = 0; k < N / 2; ++k) twiddle_[size_t(k)] = std::polar(1.0, -2 * std::numbers::pi * k / N);
    spec_.resize(N);
    reset();
}

void Denoiser::reset() {
    for (int ch = 0; ch < 2; ++ch) {
        in_[size_t(ch)].assign(N, 0.f);
        acc_[size_t(ch)].assign(N, 0.f);
        stage_[size_t(ch)].assign(H, 0.f);
        ready_[size_t(ch)].assign(H, 0.f);
        noise_[size_t(ch)].assign(Bins, 0.0);
        mean_[size_t(ch)].assign(Bins, 0.0);
        smooth_[size_t(ch)].assign(Bins, 0.0);
        gain_[size_t(ch)].assign(Bins, 1.0);
    }
    pos_ = 0;
    frames_ = 0;
}

namespace {
void fft(std::vector<std::complex<double>>& a, const std::vector<int>& rev, const std::vector<std::complex<double>>& tw,
         bool inverse) {
    const int n = int(a.size());
    for (int i = 0; i < n; ++i)
        if (i < rev[size_t(i)]) std::swap(a[size_t(i)], a[size_t(rev[size_t(i)])]);
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < len / 2; ++j) {
                std::complex<double> w = tw[size_t(j * step)];
                if (inverse) w = std::conj(w);
                const std::complex<double> u = a[size_t(i + j)], v = a[size_t(i + j + len / 2)] * w;
                a[size_t(i + j)] = u + v;
                a[size_t(i + j + len / 2)] = u - v;
            }
    }
}
} // namespace

void Denoiser::frame(int ch, double floorGain) {
    auto& in = in_[size_t(ch)];
    for (int i = 0; i < N; ++i) spec_[size_t(i)] = double(in[size_t(i)] * window_[size_t(i)]);
    fft(spec_, bitrev_, twiddle_, false);
    auto& noise = noise_[size_t(ch)];
    auto& mean = mean_[size_t(ch)];
    auto& sm = smooth_[size_t(ch)];
    auto& g = gain_[size_t(ch)];
    const bool warm = frames_ < 24;  // first ~0.06 s: learn the floor before cutting
    for (int k = 0; k < Bins; ++k) {
        const double p = std::norm(spec_[size_t(k)]);
        sm[size_t(k)] = frames_ == 0 ? p : 0.9 * sm[size_t(k)] + 0.1 * p;
        // noise floor: follows quiet energy down at once and rises ~3 dB/s, so speech does not lift it
        double& nf = noise[size_t(k)];
        if (frames_ < 24 || sm[size_t(k)] < nf) nf = sm[size_t(k)];  // (warm-up: just follow)
        else nf *= sm[size_t(k)] > 30.0 * nf ? 1.02 : 1.0018;     // far below everything: catch up
        // frames near the floor are noise: their average is the noise level to remove
        double& nm = mean[size_t(k)];
        if (frames_ < 24) nm = sm[size_t(k)];
        else if (sm[size_t(k)] < 4.0 * nf) nm = 0.97 * nm + 0.03 * sm[size_t(k)];
        // spectral subtraction (over-subtracting 2x), time-smoothed against musical noise
        const double snr = sm[size_t(k)] / std::max(nm, 1e-20);
        double target = std::clamp(1.0 - 2.0 / std::max(snr, 1e-9), floorGain, 1.0);
        if (warm) target = 1.0;
        g[size_t(k)] = target > g[size_t(k)] ? 0.5 * g[size_t(k)] + 0.5 * target : 0.85 * g[size_t(k)] + 0.15 * target;
        spec_[size_t(k)] *= g[size_t(k)];
        if (k > 0 && k < N / 2) spec_[size_t(N - k)] *= g[size_t(k)];
    }
    fft(spec_, bitrev_, twiddle_, true);
    auto& acc = acc_[size_t(ch)];
    const double norm = 1.0 / (N * 2.0);  // inverse FFT scale and the Hann overlap sum at 75 %
    for (int i = 0; i < N; ++i) acc[size_t(i)] += float(spec_[size_t(i)].real() * window_[size_t(i)] * norm);
}

void Denoiser::process(float* buf, int n, double reductionDb) {
    const double floorGain = std::pow(10.0, -std::clamp(reductionDb, 0.0, 40.0) / 20.0);
    for (int i = 0; i < n; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            const float x = buf[i * 2 + ch];
            buf[i * 2 + ch] = ready_[size_t(ch)][size_t(pos_)];
            stage_[size_t(ch)][size_t(pos_)] = x;
        }
        if (++pos_ < H) continue;
        pos_ = 0;
        for (int ch = 0; ch < 2; ++ch) {
            auto& in = in_[size_t(ch)];
            auto& acc = acc_[size_t(ch)];
            std::copy(in.begin() + H, in.end(), in.begin());  // history moves by one hop
            std::copy(stage_[size_t(ch)].begin(), stage_[size_t(ch)].end(), in.end() - H);
            frame(ch, floorGain);
            // the oldest hop has had all its overlapping frames now: it is the next output
            std::copy(acc.begin(), acc.begin() + H, ready_[size_t(ch)].begin());
            std::copy(acc.begin() + H, acc.end(), acc.begin());
            std::fill(acc.end() - H, acc.end(), 0.f);
        }
        ++frames_;
    }
}
} // namespace cut
