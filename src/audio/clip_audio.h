#pragma once
// Audio of one clip at timeline sample positions (48 kHz stereo float).
//  - speed 1: direct sample-accurate reads,
//  - speed != 1: pitch-preserving time stretch through a streaming FFmpeg `atempo` chain
//    (re-primed whenever the read position jumps),
//  - reverse: source read in 1 s chunks and played backwards,
//  - hold (speed 0): silence.
#include "core/model.h"
#include "media/frame_reader.h"

#include <memory>
#include <vector>

namespace cut {

class TempoStretcher;

class ClipAudio {
public:
    ClipAudio();
    ~ClipAudio();
    bool open(const std::string& path, int streamIndex, Tick origin, std::string& error);
    // Renders n frames for timeline samples [ts, ts+n). Positions outside the clip extrapolate the
    // source (transition handles); outside the media is silence.
    void render(const Clip& c, int64_t ts, float* out, int n);

private:
    void renderForward(int64_t srcSample, float* out, int n);
    void renderReverse(int64_t srcEndSample, float* out, int n);  // samples [end-n, end) reversed
    AudioReader reader_;
    std::unique_ptr<TempoStretcher> tempo_;
    double tempoSpeed_ = 0;
    bool tempoReverse_ = false;
    int64_t expectedTs_ = INT64_MIN;
    int64_t feedPos_ = 0;  // next source sample to feed the stretcher (forward) / end (reverse)
    // reverse chunk cache
    std::vector<float> chunk_;
    int64_t chunkStart_ = 0, chunkEnd_ = 0;
    std::vector<float> tmp_;
};

// Streaming tempo change (pitch preserved), interleaved stereo float in/out.
class TempoStretcher {
public:
    explicit TempoStretcher(double speed);
    ~TempoStretcher();
    bool ok() const;
    void push(const float* in, int frames);
    // Pulls up to n frames; returns frames produced.
    int pull(float* out, int n);
    void flushInput();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cut
