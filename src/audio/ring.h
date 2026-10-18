#pragma once
// Single-producer / single-consumer lock-free ring of interleaved stereo float frames.
// The audio thread is the consumer: no locks, no allocation.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cut {

class StereoRing {
public:
    explicit StereoRing(size_t frames = 48000) { reset(frames); }
    void reset(size_t frames) {
        buf_.assign(frames * 2, 0.f);
        cap_ = frames;
        r_.store(0);
        w_.store(0);
    }
    size_t capacity() const { return cap_; }
    size_t available() const { return size_t(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)); }
    size_t space() const { return cap_ - available(); }

    size_t write(const float* src, size_t frames) {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        frames = std::min(frames, cap_ - size_t(w - r));
        for (size_t i = 0; i < frames;) {
            size_t pos = (w + i) % cap_;
            size_t n = std::min(frames - i, cap_ - pos);
            std::memcpy(&buf_[pos * 2], src + i * 2, n * 2 * sizeof(float));
            i += n;
        }
        w_.store(w + frames, std::memory_order_release);
        return frames;
    }

    size_t read(float* dst, size_t frames) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        frames = std::min(frames, size_t(w - r));
        for (size_t i = 0; i < frames;) {
            size_t pos = (r + i) % cap_;
            size_t n = std::min(frames - i, cap_ - pos);
            std::memcpy(dst + i * 2, &buf_[pos * 2], n * 2 * sizeof(float));
            i += n;
        }
        r_.store(r + frames, std::memory_order_release);
        return frames;
    }
    // Consumer side: drop everything currently buffered.
    void discard() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }
    // Consumer-side position (total frames ever read).
    uint64_t readCount() const { return r_.load(std::memory_order_acquire); }
    uint64_t writeCount() const { return w_.load(std::memory_order_acquire); }

    // Only when neither side is running.
    void clear() {
        r_.store(0);
        w_.store(0);
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0;
    std::atomic<uint64_t> r_{0}, w_{0};
};

} // namespace cut
