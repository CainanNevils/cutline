#pragma once
// Integrated loudness (ITU-R BS.1770-4 / EBU R128) of 48 kHz stereo audio: K-weighting,
// 400 ms blocks with 75 % overlap, absolute gate -70 LUFS, relative gate -10 LU.
#include <cstdint>
#include <vector>

namespace cut {

class LoudnessMeter {
public:
    LoudnessMeter();
    void push(const float* interleavedStereo, int frames);
    // Integrated loudness in LUFS (-inf when silent).
    double integrated() const;
    double samplePeak() const { return peak_; }
    // EBU R128 momentary (400 ms) and short-term (3 s) loudness of the most recent audio.
    double momentary() const { return windowed(4); }
    double shortTerm() const { return windowed(30); }
    void reset();

private:
    double windowed(int steps) const;
    struct Biquad {
        double b0, b1, b2, a1, a2;
        double z1 = 0, z2 = 0;
        double run(double x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Biquad shelf_[2], hp_[2];
    std::vector<double> stepEnergy_;  // energy of each 100 ms step (sum of channels' mean squares)
    double acc_ = 0;
    int accN_ = 0;
    double peak_ = 0;
};

} // namespace cut
