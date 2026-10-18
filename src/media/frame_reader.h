#pragma once
// PTS-accurate frame access on top of VideoDecoder.
//  - get(t) returns the frame shown at media time t: the frame with the greatest pts <= t+tol
//    (VFR-safe; tol absorbs container timestamp rounding, see DECISIONS D4).
//  - Forward requests near the decode position decode forward; anything else seeks to the
//    previous keyframe and decodes forward, copying only frames inside the window to the GPU.
//  - Requests just below the window (reverse playback/export) refill a window that ends at t,
//    so reversing costs about one GOP decode per `window` frames and memory stays bounded.
#include "media/video_decoder.h"

#include <deque>
#include <functional>
#include <mutex>

namespace cut {

class FrameReader {
public:
    struct Stats {
        uint64_t seeks = 0, decoded = 0, kept = 0;
        double lastSeekMs = 0;
    };
    bool open(const std::string& path, std::shared_ptr<GpuDevice> gpu, const MediaInfo& info,
              const VideoDecoder::Options& opt, std::string& error);
    bool isOpen() const { return dec_.isOpen() || still_ != nullptr; }
    bool isHw() const { return dec_.isHw(); }
    std::string describe() const { return dec_.describe(); }

    void setWindow(int frames) { window_ = std::max(4, frames); }
    int window() const { return window_; }
    void setTolerance(Tick tol) { tol_ = tol; }
    Tick frameDuration() const { return frameDur_; }

    // Decodes as needed. `cancel` is polled between frames (latest-request-wins seeking).
    GpuFramePtr get(Tick t, const std::function<bool()>& cancel = {});
    // Cached lookup only.
    GpuFramePtr peek(Tick t) const;
    // Frame following `f` if it is in the window (frame blending).
    GpuFramePtr nextAfter(const GpuFramePtr& f) const;
    Tick decodePosition() const { return lastPts_; }
    std::vector<GpuFramePtr> frames() const;
    void clearWindow();
    const Stats& stats() const { return stats_; }

private:
    GpuFramePtr findLocked(Tick t) const;
    void insert(const GpuFramePtr& f, Tick focus);
    bool seekTo(Tick target);

    VideoDecoder dec_;
    mutable std::mutex mutex_;  // guards win_ (peek() may run on another thread)
    std::deque<GpuFramePtr> win_;
    GpuFramePtr still_;
    GpuFramePtr lastKept_;
    Tick lastPts_ = kInvalidTick;
    Tick mediaDur_ = 0;
    Tick frameDur_ = kTicksPerSecond / 30;
    Tick tol_ = kTicksPerSecond / 120;
    int window_ = 48;
    bool image_ = false;
    Stats stats_;
};

// Interleaved stereo float audio at 48 kHz from one audio stream, sample-position addressed.
class AudioReader {
public:
    AudioReader();
    ~AudioReader();
    bool open(const std::string& path, int streamIndex, Tick origin, std::string& error);
    bool isOpen() const;
    // Fills `frames` stereo frames starting at media sample `pos` (48 kHz). Silence outside the
    // media. Sequential reads are cheap; anything else seeks.
    void read(int64_t pos, float* out, int frames);
    int64_t lengthSamples() const { return length_; }

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    int64_t length_ = 0;
};

} // namespace cut
