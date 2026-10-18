#include "audio/audio_fx.h"

#include "audio/denoise.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cut {

namespace {

constexpr double kFs = 48000.0;

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

void normalizeCoeffs(ClipFxChain::Biquad& q, double b0, double b1, double b2, double a0, double a1, double a2) {
    q.b0 = b0 / a0;
    q.b1 = b1 / a0;
    q.b2 = b2 / a0;
    q.a1 = a1 / a0;
    q.a2 = a2 / a0;
}

// one-pole smoothing coefficient for a time constant in milliseconds
double timeCoeff(double ms) { return std::exp(-1.0 / (std::max(0.01, ms) * 0.001 * kFs)); }

} // namespace

void ClipFxChain::Biquad::highPass(double f, double q) {
    const double w = 2 * std::numbers::pi * std::clamp(f, 10.0, 20000.0) / kFs, c = std::cos(w), al = std::sin(w) / (2 * q);
    normalizeCoeffs(*this, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

void ClipFxChain::Biquad::lowPass(double f, double q) {
    const double w = 2 * std::numbers::pi * std::clamp(f, 10.0, 22000.0) / kFs, c = std::cos(w), al = std::sin(w) / (2 * q);
    normalizeCoeffs(*this, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

void ClipFxChain::Biquad::peak(double f, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40), w = 2 * std::numbers::pi * f / kFs, c = std::cos(w),
                 al = std::sin(w) / (2 * q);
    normalizeCoeffs(*this, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

void ClipFxChain::Biquad::lowShelf(double f, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40), w = 2 * std::numbers::pi * f / kFs, c = std::cos(w),
                 al = std::sin(w) / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
    normalizeCoeffs(*this, A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
                    (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
}

void ClipFxChain::Biquad::highShelf(double f, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40), w = 2 * std::numbers::pi * f / kFs, c = std::cos(w),
                 al = std::sin(w) / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
    normalizeCoeffs(*this, A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
                    (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
}

bool isAudioEffect(const std::string& type) {
    return type == fx::HighPass || type == fx::LowPass || type == fx::Eq || type == fx::Compressor || type == fx::NoiseGate ||
           type == fx::Denoise;
}

int ClipFxChain::latency(const Clip& c) {
    for (const auto& e : c.effects)
        if (e.enabled && e.type == fx::Denoise) return Denoiser::kLatency;
    return 0;
}

bool hasAudioEffects(const Clip& c) {
    for (const auto& e : c.effects)
        if (e.enabled && isAudioEffect(e.type)) return true;
    return false;
}

void ClipFxChain::process(const Clip& c, Tick local, float* buf, int n) {
    // keep one slot per effect position; a changed stack (added / removed / reordered) resets it
    size_t si = 0;
    for (const Effect& e : c.effects) {
        if (!e.enabled || !isAudioEffect(e.type)) continue;
        if (si >= slots_.size() || slots_[si].type != e.type) {
            slots_.resize(si);
            slots_.push_back(Slot{e.type});
        }
        Slot& s = slots_[si++];
        if (e.type == fx::HighPass || e.type == fx::LowPass) {
            const double f = e.p("frequency", local);
            if (e.type == fx::HighPass) s.bq[0].highPass(f);
            else s.bq[0].lowPass(f);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < 2; ++ch) buf[i * 2 + ch] = s.bq[0].run(ch, buf[i * 2 + ch]);
        } else if (e.type == fx::Eq) {
            s.bq[0].lowShelf(120, e.p("low", local));
            s.bq[1].peak(std::clamp(e.p("midFreq", local), 100.0, 10000.0), 1.0, e.p("mid", local));
            s.bq[2].highShelf(8000, e.p("high", local));
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    float x = buf[i * 2 + ch];
                    for (auto& q : s.bq) x = q.run(ch, x);
                    buf[i * 2 + ch] = x;
                }
        } else if (e.type == fx::Compressor) {
            // feed-forward, stereo-linked peak detector with attack / release, hard knee
            const double thr = e.p("threshold", local), ratio = std::max(1.0, e.p("ratio", local));
            const double att = timeCoeff(e.p("attack", local)), rel = timeCoeff(e.p("release", local));
            const double makeup = dbToLin(e.p("makeup", local));
            for (int i = 0; i < n; ++i) {
                const double x = std::max(std::abs(buf[i * 2]), std::abs(buf[i * 2 + 1]));
                s.env = x > s.env ? att * s.env + (1 - att) * x : rel * s.env + (1 - rel) * x;
                const double envDb = 20 * std::log10(std::max(s.env, 1e-9));
                const double over = envDb - thr;
                const double g = (over > 0 ? dbToLin(-over * (1 - 1 / ratio)) : 1.0) * makeup;
                buf[i * 2] = float(buf[i * 2] * g);
                buf[i * 2 + 1] = float(buf[i * 2 + 1] * g);
            }
        } else if (e.type == fx::Denoise) {
            if (!s.dn) s.dn = std::make_shared<Denoiser>();
            s.dn->process(buf, n, e.p("reduction", local));
        } else if (e.type == fx::NoiseGate) {
            // opens fast (1 ms), holds 50 ms, then closes over `release` down to `reduction`
            const double thr = dbToLin(e.p("threshold", local));
            const double floorGain = dbToLin(e.p("reduction", local));
            const double open = timeCoeff(1.0), close = timeCoeff(e.p("release", local));
            const double det = timeCoeff(5.0);
            for (int i = 0; i < n; ++i) {
                const double x = std::max(std::abs(buf[i * 2]), std::abs(buf[i * 2 + 1]));
                s.env = std::max(x, det * s.env);
                double target = floorGain;
                if (s.env >= thr) {
                    target = 1.0;
                    s.hold = int(0.05 * kFs);
                } else if (s.hold > 0) {
                    --s.hold;
                    target = 1.0;
                }
                const double k = target > s.gateGain ? open : close;
                s.gateGain = k * s.gateGain + (1 - k) * target;
                buf[i * 2] = float(buf[i * 2] * s.gateGain);
                buf[i * 2 + 1] = float(buf[i * 2 + 1] * s.gateGain);
            }
        }
    }
    slots_.resize(si);
}

} // namespace cut
