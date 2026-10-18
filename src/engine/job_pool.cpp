#include "engine/job_pool.h"

#include <windows.h>

#include <algorithm>

namespace cut {

bool JobToken::checkpoint() const {
    if (pool_) pool_->waitWhilePaused();
    return !cancel_.load();
}

JobPool::JobPool(int workers) {
    if (workers <= 0) workers = std::clamp(int(std::thread::hardware_concurrency()) / 4, 2, 4);
    for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { worker(false); });
    threads_.emplace_back([this] { worker(true); });
}

JobPool::~JobPool() {
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
        for (auto& j : general_) j.token->cancel();
        for (auto& j : proxy_) j.token->cancel();
    }
    paused_ = false;
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

JobTokenPtr JobPool::submit(std::string name, std::function<void(JobToken&)> fn, Lane lane, bool front) {
    auto tok = std::make_shared<JobToken>();
    tok->pool_ = this;
    {
        std::lock_guard<std::mutex> l(m_);
        auto& q = lane == Lane::Proxy ? proxy_ : general_;
        Job j{std::move(name), std::move(fn), tok, lane};
        if (front) q.push_front(std::move(j));
        else q.push_back(std::move(j));
    }
    cv_.notify_all();
    return tok;
}

void JobPool::setPaused(bool p) {
    paused_ = p;
    cv_.notify_all();
}

int JobPool::pending() const {
    std::lock_guard<std::mutex> l(m_);
    return int(general_.size() + proxy_.size());
}

void JobPool::cancelAll() {
    std::lock_guard<std::mutex> l(m_);
    for (auto& j : general_) j.token->cancel();
    for (auto& j : proxy_) j.token->cancel();
    general_.clear();
    proxy_.clear();
}

void JobPool::waitWhilePaused() const {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [&] { return !paused_.load() || quit_; });
}

void JobPool::worker(bool proxyLane) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(m_);
            auto& q = proxyLane ? proxy_ : general_;
            cv_.wait(l, [&] { return quit_ || (!paused_.load() && !q.empty()); });
            if (quit_) return;
            job = std::move(q.front());
            q.pop_front();
        }
        if (job.token->cancelled()) continue;
        ++running_;
        try {
            job.fn(*job.token);
        } catch (...) {
            // a failing background job must never take the app down
        }
        --running_;
    }
}

} // namespace cut
