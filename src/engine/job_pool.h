#pragma once
// Low-priority background jobs (probing, thumbnails, waveforms, proxies). Workers run at
// below-normal priority and the pool can be paused (while playing / exporting) so playback never
// competes with background work. Jobs poll their token to cancel or pause.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cut {

class JobToken {
public:
    bool cancelled() const { return cancel_.load(); }
    void cancel() { cancel_ = true; }
    // Blocks while the pool is paused; returns false if the job should stop.
    bool checkpoint() const;
    std::atomic<double> progress{0};

private:
    friend class JobPool;
    std::atomic<bool> cancel_{false};
    class JobPool* pool_ = nullptr;
};
using JobTokenPtr = std::shared_ptr<JobToken>;

class JobPool {
public:
    enum class Lane { General, Proxy };  // proxy lane runs one job at a time (NVENC sessions)
    explicit JobPool(int workers = 0);
    ~JobPool();
    JobTokenPtr submit(std::string name, std::function<void(JobToken&)> fn, Lane lane = Lane::General,
                       bool front = false);
    void setPaused(bool p);
    bool paused() const { return paused_.load(); }
    int pending() const;
    int running() const { return running_.load(); }
    void cancelAll();
    // Blocks while paused (used by JobToken::checkpoint).
    void waitWhilePaused() const;

private:
    struct Job {
        std::string name;
        std::function<void(JobToken&)> fn;
        JobTokenPtr token;
        Lane lane;
    };
    void worker(bool proxyLane);
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::deque<Job> general_, proxy_;
    std::vector<std::thread> threads_;
    std::atomic<bool> paused_{false};
    std::atomic<int> running_{0};
    bool quit_ = false;
};

} // namespace cut
