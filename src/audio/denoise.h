#pragma once
// Spectral noise reduction for voice (stereo, 48 kHz): 512-point STFT with 75 % overlap,
// per-bin noise floor tracked from the quietest recent energy (no noise print needed), smoothed
// Wiener-style gains limited to `reduction` dB. Output lags the input by kLatency samples; the
// mixer reads such clips that far ahead so they stay in sync.
#include <array>
#include <complex>
#include <vector>

namespace cut {

class Denoiser {
public:
    static constexpr int kSize = 512, kHop = 128, kLatency = kSize;
    Denoiser();
    // In place, interleaved stereo, any n.
    void process(float* buf, int n, double reductionDb);
    void reset();

private:
    void frame(int ch, double floorGain);
    // in_: last N input samples (time order); acc_: overlap-add output aligned with in_;
    // stage_: input of the hop being collected; ready_: finished output of the previous hop
    std::array<std::vector<float>, 2> in_, acc_, stage_, ready_;
    int pos_ = 0;  // position within the current hop
    std::vector<float> window_;
    std::array<std::vector<double>, 2> noise_, mean_, smooth_, gain_;  // floor (minimum), noise mean, power, gain
    std::vector<std::complex<double>> spec_;
    std::vector<int> bitrev_;
    std::vector<std::complex<double>> twiddle_;
    long long frames_ = 0;
};

} // namespace cut
