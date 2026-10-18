#pragma once
// Asynchronous frame source for preview: one decoder thread per (clip, media) so playback across
// cuts never waits on a seek (the next clip's source starts decoding before the cut).
// The render thread never blocks: it calls request() and peek() and holds the last frame when
// a frame is not ready yet. Seeks are cancellable: the latest request wins.
#include "media/frame_reader.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace cut {

class VideoSource {
public:
    VideoSource(std::shared_ptr<GpuDevice> gpu, std::string path, MediaInfo info, bool allowHw, int window);
    ~VideoSource();

    // Asks for media time t; `ahead` extra frames are decoded after it (playback lookahead).
    void request(Tick t, int ahead);
    GpuFramePtr peek(Tick t) const { return reader_.peek(t); }
    GpuFramePtr nextAfter(const GpuFramePtr& f) const { return reader_.nextAfter(f); }
    // Closest decoded frame (for "hold last frame" when the exact one is late).
    GpuFramePtr nearest(Tick t) const;

    bool ready() const { return opened_.load(); }
    bool failed() const { return failed_.load(); }
    std::string error() const;
    bool isHw() const { return hw_.load(); }
    double lastSeekMs() const { return lastSeekMs_.load(); }
    bool idle() const { return idle_.load(); }
    void setWindow(int frames);
    // A short clip never needs more cached frames than it can show (plus handles).
    void setMaxWindow(int frames);
    // Bytes of one decoded frame of this media (NV12 / P010).
    size_t frameBytesEstimate() const {
        const size_t px = size_t(std::max(2, info_.video.width)) * size_t(std::max(2, info_.video.height));
        return info_.video.bitDepth > 8 ? px * 3 : px * 3 / 2;
    }
    const std::string& path() const { return path_; }
    Tick frameDuration() const { return frameDur_; }

    std::atomic<int64_t> lastUsedMs{0};  // set by the owner (render thread)

private:
    void run();
    std::shared_ptr<GpuDevice> gpu_;
    std::string path_;
    MediaInfo info_;
    bool allowHw_;
    FrameReader reader_;
    Tick frameDur_;
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    Tick target_ = 0;
    int ahead_ = 0;
    uint64_t gen_ = 0;
    std::atomic<uint64_t> genAtomic_{0};
    std::atomic<Tick> targetAtomic_{0};
    bool quit_ = false;
    std::atomic<bool> opened_{false}, failed_{false}, hw_{false}, idle_{true};
    std::atomic<double> lastSeekMs_{0};
    std::string error_;
    std::atomic<int> window_{48};
    std::atomic<int> maxWindow_{48};
};

} // namespace cut
