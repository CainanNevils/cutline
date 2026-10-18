#pragma once
// Lining up two recordings of the same moment (gameplay capture + a separately recorded mic or
// camera) by their sound: onset envelopes are cross-correlated with an FFT over the whole length
// (coarse, 5 ms), then the lag is sharpened on the audio itself around that point.
#include <vector>

namespace cut {

struct SyncMatch {
    bool ok = false;
    double lagSec = 0;      // y(t) ~ x(t + lag)
    double confidence = 0;  // peak / best peak elsewhere (>= ~1.3 is a clear match)
};

// x, y: level envelopes (e.g. peak per 5 ms) at envRate values per second.
SyncMatch matchEnvelopes(const std::vector<float>& x, const std::vector<float>& y, double envRate);

// Best lag (samples) with y[i] ~ x[i + lag] for lag in [lo, hi]; mono signals. Ignores polarity.
int refineLag(const std::vector<float>& x, const std::vector<float>& y, int lo, int hi);

} // namespace cut
