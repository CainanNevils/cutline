#include "engine/video_source.h"

#include <algorithm>

#include <windows.h>

namespace cut {

VideoSource::VideoSource(std::shared_ptr<GpuDevice> gpu, std::string path, MediaInfo info, bool allowHw, int window)
    : gpu_(std::move(gpu)), path_(std::move(path)), info_(std::move(info)), allowHw_(allowHw) {
    frameDur_ = info_.video.fps.valid() ? cut::frameDuration(info_.video.fps) : kTicksPerSecond / 30;
    window_ = window;
    thread_ = std::thread([this] { run(); });
}

VideoSource::~VideoSource() {
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
        ++gen_;
        genAtomic_ = gen_;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::string VideoSource::error() const {
    std::lock_guard<std::mutex> l(m_);
    return error_;
}

void VideoSource::setWindow(int frames) { window_ = std::clamp(frames, 8, maxWindow_.load()); }

void VideoSource::setMaxWindow(int frames) {
    maxWindow_ = std::max(8, frames);
    window_ = std::min(window_.load(), maxWindow_.load());
}

void VideoSource::request(Tick t, int ahead) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (t == target_ && ahead == ahead_ && !idle_) return;
        target_ = t;
        ahead_ = ahead;
        ++gen_;
        genAtomic_ = gen_;
        targetAtomic_ = t;
    }
    cv_.notify_one();
}

GpuFramePtr VideoSource::nearest(Tick t) const {
    auto frames = reader_.frames();
    GpuFramePtr best;
    Tick bestD = INT64_MAX;
    for (const auto& f : frames) {
        Tick d = std::llabs(f->pts - t);
        if (d < bestD) {
            bestD = d;
            best = f;
        }
    }
    return best;
}

void VideoSource::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    {
        std::string err;
        VideoDecoder::Options o;
        o.allowHw = allowHw_;
        if (!reader_.open(path_, gpu_, info_, o, err)) {
            std::lock_guard<std::mutex> l(m_);
            error_ = err;
            failed_ = true;
            return;
        }
        reader_.setWindow(window_);
        hw_ = reader_.isHw();
        opened_ = true;
    }
    uint64_t done = 0;
    for (;;) {
        Tick target;
        int ahead;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> l(m_);
            idle_ = true;
            cv_.wait(l, [&] { return quit_ || gen_ != done; });
            if (quit_) return;
            idle_ = false;
            target = target_;
            ahead = ahead_;
            gen = gen_;
        }
        reader_.setWindow(window_);
        const Tick fd = frameDur_;
        // Cancel only when the newest request is somewhere this decode is not heading
        // (backwards, or far ahead); small forward moves just continue (playback, slow scrubs).
        auto cancel = [&] {
            if (genAtomic_.load() == gen) return false;
            Tick nt = targetAtomic_.load();
            return nt < target - fd * 2 || nt > target + 2 * kTicksPerSecond;
        };
        reader_.get(target, cancel);
        lastSeekMs_ = reader_.stats().lastSeekMs;
        hw_ = reader_.isHw();
        for (int i = 1; i <= ahead; ++i) {
            if (genAtomic_.load() != gen) break;  // new request: re-plan from it
            if (info_.duration > 0 && target + fd * i >= info_.duration) break;
            reader_.get(target + fd * i, cancel);
        }
        done = gen;
    }
}

} // namespace cut
