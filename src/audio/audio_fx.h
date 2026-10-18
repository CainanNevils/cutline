#pragma once
// Clip audio effects (48 kHz stereo, in place, before the clip's volume / fades):
// high-pass and low-pass filters, 3-band parametric EQ, compressor, noise gate and spectral DeNoise.
// Parameters can be keyframed; they are read once per processed block. Filter / envelope state carries over
// between consecutive blocks and is reset when playback jumps.
#include "core/model.h"

#include <memory>
#include <string>
#include <vector>

namespace cut {

class Denoiser;

bool isAudioEffect(const std::string& type);
bool hasAudioEffects(const Clip& c);

class ClipFxChain {
public:
    // buf: interleaved stereo, n frames, starting at clip-local time `local`.
    void process(const Clip& c, Tick local, float* buf, int n);
    void reset() { slots_.clear(); }
    // Samples the chain delays its output by (DeNoise); the mixer reads the clip that far ahead.
    static int latency(const Clip& c);

    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {0, 0}, z2[2] = {0, 0};
        float run(int ch, float x) {
            const double y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return float(y);
        }
        // RBJ audio EQ cookbook designs (fs = 48 kHz)
        void highPass(double f, double q = 0.70710678);
        void lowPass(double f, double q = 0.70710678);
        void peak(double f, double q, double gainDb);
        void lowShelf(double f, double gainDb);
        void highShelf(double f, double gainDb);
    };

private:
    struct Slot {
        std::string type;
        Biquad bq[3];
        double env = 0;       // compressor / gate detector (linear)
        double gateGain = 1;  // current gate gain
        int hold = 0;         // gate hold samples left
        std::shared_ptr<Denoiser> dn;
    };
    std::vector<Slot> slots_;
};

} // namespace cut
