#include "audio/loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cut {

LoudnessMeter::LoudnessMeter() {
    // BS.1770-4 K-weighting coefficients for 48 kHz
    for (int c = 0; c < 2; ++c) {
        shelf_[c] = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
        hp_[c] = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    }
}

void LoudnessMeter::push(const float* in, int frames) {
    const int step = 4800;  // 100 ms
    for (int i = 0; i < frames; ++i) {
        double e = 0;
        for (int c = 0; c < 2; ++c) {
            double x = in[size_t(i) * 2 + c];
            peak_ = std::max(peak_, std::abs(x));
            double y = hp_[c].run(shelf_[c].run(x));
            e += y * y;  // G = 1 for L and R
        }
        acc_ += e;
        if (++accN_ == step) {
            stepEnergy_.push_back(acc_ / step);
            acc_ = 0;
            accN_ = 0;
        }
    }
}

double LoudnessMeter::integrated() const {
    // 400 ms blocks = 4 consecutive 100 ms steps (75 % overlap)
    std::vector<double> blocks;
    for (size_t i = 0; i + 4 <= stepEnergy_.size(); ++i)
        blocks.push_back((stepEnergy_[i] + stepEnergy_[i + 1] + stepEnergy_[i + 2] + stepEnergy_[i + 3]) / 4.0);
    auto lufs = [](double z) { return z > 0 ? -0.691 + 10.0 * std::log10(z) : -std::numeric_limits<double>::infinity(); };
    double sum = 0;
    int n = 0;
    for (double z : blocks)
        if (lufs(z) > -70.0) {
            sum += z;
            ++n;
        }
    if (!n) return -std::numeric_limits<double>::infinity();
    const double relGate = lufs(sum / n) - 10.0;
    sum = 0;
    n = 0;
    for (double z : blocks)
        if (lufs(z) > -70.0 && lufs(z) > relGate) {
            sum += z;
            ++n;
        }
    return n ? lufs(sum / n) : -std::numeric_limits<double>::infinity();
}

double LoudnessMeter::windowed(int steps) const {
    if (int(stepEnergy_.size()) < steps) return -std::numeric_limits<double>::infinity();
    double z = 0;
    for (size_t i = stepEnergy_.size() - size_t(steps); i < stepEnergy_.size(); ++i) z += stepEnergy_[i];
    z /= steps;
    return z > 0 ? -0.691 + 10.0 * std::log10(z) : -std::numeric_limits<double>::infinity();
}

void LoudnessMeter::reset() {
    *this = LoudnessMeter();
}

} // namespace cut