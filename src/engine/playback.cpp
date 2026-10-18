#include "engine/playback.h"

#include "render/gpu_timer.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <deque>

#pragma comment(lib, "winmm.lib")

namespace cut {

namespace {
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
int64_t nowMs() { return nowUs() / 1000; }
constexpr int kMixBlock = 480;      // 10 ms
constexpr int kRingTarget = 3840;   // ~80 ms of mixed audio ahead of the device
constexpr int kStartThreshold = 1920;
} // namespace

struct PlaybackEngine::Viewer {
    int id = 0;
    // window (UI -> render thread)
    HWND hwnd = nullptr;
    int w = 0, h = 0;
    bool windowDirty = false;
    SwapChain sc;
    // content
    SnapshotPtr snap;
    ViewSettings view;
    Tick pos = 0;
    Tick previewOverride = kInvalidTick;
    Tick playEnd = kInvalidTick;
    bool dirty = true;
    bool incomplete = false;
    int64_t incompleteSinceMs = 0;
    Tick lastRendered = kInvalidTick;
    // canvas / quality
    GpuTexturePtr canvas;
    int autoDiv = 1;
    int slow = 0, fast = 0;
    // hint shown centred when the content is empty
    std::vector<uint8_t> hint, rhint;
    int hintW = 0, hintH = 0, rhintW = 0, rhintH = 0;
    bool hintDirty = false, rhintDirty = false;
    GpuTexturePtr hintTex;
    // waveform shown (stretched, with a playhead) when the content has audio but no video
    std::vector<uint8_t> wave, rwave;
    int waveW = 0, waveH = 0, rwaveW = 0, rwaveH = 0;
    bool waveDirty = false, rwaveDirty = false;
    GpuTexturePtr waveTex;
    // HUD
    std::vector<uint8_t> hud;
    int hudW = 0, hudH = 0;
    bool hudDirty = false;
    GpuTexturePtr hudTex;
    // render-thread copies (taken under the lock each iteration)
    SnapshotPtr rsnap;
    ViewSettings rview;
    std::vector<uint8_t> rhud;
    int rhudW = 0, rhudH = 0;
    bool rhudDirty = false;
    // stats: rstats is private to the render thread; stats is the published copy (guarded by m_)
    ViewerStats rstats;
    ViewerStats stats;
    bool resetStats = false;
    GpuTimer timer;
    bool timerInit = false;
    std::deque<int64_t> shownAt;
    // scopes: a small copy of the composited frame read back asynchronously
    std::atomic<bool> scopes{false};
    GpuTexturePtr scopeRT;
    ComPtr<ID3D11Texture2D> scopeStaging;
    bool scopePending = false;
    int64_t lastScopeMs = 0;
};

namespace {
// The clip right before c on its track when c simply continues it (same media, touching, no
// transition): pieces of a split clip or speed ramp, which can inherit its decoder.
const Clip* continuedFrom(const Sequence& s, const Clip& c) {
    if (c.kind != ClipKind::Media) return nullptr;
    for (const auto& p : s.clips)
        if (p.id != c.id && p.trackId == c.trackId && p.end() == c.start && p.assetId == c.assetId && p.kind == ClipKind::Media)
            return s.transitionAtCut(p.id, c.id) ? nullptr : &p;
    return nullptr;
}
} // namespace

class PlaybackEngine::Provider : public FrameProvider {
public:
    Provider(PlaybackEngine& e, Viewer& v, const RenderSnapshot& s, bool playing)
        : e_(e), v_(v), s_(s), playing_(playing) {}

    std::shared_ptr<VideoSource> source(const Clip& c, const AssetRef& a) {
        const bool proxy = s_.useProxies && !a.proxyPath.empty();
        SourceKey key{v_.id, c.id, proxy};
        auto it = e_.sources_.find(key);
        if (it == e_.sources_.end())
            if (const Clip* prev = continuedFrom(s_.seq, c)) {
                // take over the previous piece's decoder: no new decoder, no seek at the cut
                auto pit = e_.sources_.find({v_.id, prev->id, proxy});
                if (pit != e_.sources_.end()) {
                    auto src = pit->second;
                    e_.sources_.erase(pit);
                    e_.held_.erase({v_.id, prev->id});
                    const MediaInfo& mi = proxy ? a.proxyInfo : a.info;
                    const Tick mfd = mi.video.fps.valid() ? frameDuration(mi.video.fps) : kTicksPerSecond / 60;
                    src->setMaxWindow(c.isHold() ? 8 : int(std::min<Tick>(48, c.sourceSpan() / std::max<Tick>(1, mfd) + 24)));
                    it = e_.sources_.emplace(key, std::move(src)).first;
                }
            }
        if (it == e_.sources_.end()) {
            auto src = std::make_shared<VideoSource>(e_.gpu_, proxy ? a.proxyPath : a.path, proxy ? a.proxyInfo : a.info,
                                                     true, 40);
            // frames this clip can show (+ handles for transitions and frame blending)
            const MediaInfo& mi = proxy ? a.proxyInfo : a.info;
            const Tick mfd = mi.video.fps.valid() ? frameDuration(mi.video.fps) : kTicksPerSecond / 60;
            src->setMaxWindow(c.isHold() ? 8 : int(std::min<Tick>(48, c.sourceSpan() / std::max<Tick>(1, mfd) + 24)));
            it = e_.sources_.emplace(key, std::move(src)).first;
            e_.applyBudget();
        }
        it->second->lastUsedMs = nowMs();
        return it->second;
    }

    GpuFramePtr frame(const Clip& c, const AssetRef& a, Tick src, bool& late) override {
        auto s = source(c, a);
        s->request(src, playing_ ? 12 : 0);
        GpuFramePtr f = s->peek(src);
        auto key = std::make_pair(v_.id, c.id);
        if (f) {
            e_.held_[key] = f;
            return f;
        }
        late = true;
        auto it = e_.held_.find(key);
        if (it != e_.held_.end() && it->second) return it->second;
        return s->nearest(src);
    }

    GpuFramePtr nextFrame(const Clip& c, const AssetRef& a, const GpuFramePtr& f) override {
        return source(c, a)->nextAfter(f);
    }

private:
    PlaybackEngine& e_;
    Viewer& v_;
    const RenderSnapshot& s_;
    bool playing_;
};

PlaybackEngine::PlaybackEngine() = default;
PlaybackEngine::~PlaybackEngine() { shutdown(); }

bool PlaybackEngine::start(std::string& error) {
    gpu_ = GpuDevice::create(
#ifdef CUTLINE_DEBUG
        true,
#else
        false,
#endif
        error);
    if (!gpu_) return false;
    comp_ = std::make_unique<Compositor>();
    if (!comp_->init(gpu_, error)) return false;
    renderer_ = std::make_unique<FrameRenderer>(*comp_);
    audio_ = createAudioOutput([this](float* o, int n) { audioPull(o, n); }, audioBackend_, audioError_, audioDevice_);
    renderThread_ = std::thread([this] { renderLoop(); });
    mixerThread_ = std::thread([this] { mixerLoop(); });
    return true;
}

void PlaybackEngine::shutdown() {
    {
        std::lock_guard<std::mutex> l(m_);
        if (quit_) return;
        quit_ = true;
        playing_ = false;
        session_ = 0;
    }
    cv_.notify_all();
    mixCv_.notify_all();
    if (renderThread_.joinable()) renderThread_.join();
    if (mixerThread_.joinable()) mixerThread_.join();
    if (audio_) audio_->stop();
    audio_.reset();
}

int PlaybackEngine::addViewer() {
    std::lock_guard<std::mutex> l(m_);
    auto v = std::make_unique<Viewer>();
    v->id = nextViewer_++;
    int id = v->id;
    viewers_[id] = std::move(v);
    return id;
}

void PlaybackEngine::attachWindow(int id, HWND hwnd, int w, int h) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->hwnd = hwnd;
        v->w = w;
        v->h = h;
        v->windowDirty = true;
        v->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::resizeWindow(int id, int w, int h) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        if (v->w == w && v->h == h) return;
        v->w = w;
        v->h = h;
        v->windowDirty = true;
        v->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::detachWindow(int id) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->hwnd = nullptr;
        v->windowDirty = true;
    }
    cv_.notify_all();
    // wait until the render thread released the swapchain (the HWND may be destroyed next)
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> l(m_);
            if (!viewers_.at(id)->windowDirty || quit_) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void PlaybackEngine::setContent(int id, SnapshotPtr snap) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->snap = snap;
        v->dirty = true;
        if (playing_ && playViewer_ == id) {
            std::lock_guard<std::mutex> ml(mixMutex_);
            mixSnap_ = snap;
        }
    }
    cv_.notify_all();
}

void PlaybackEngine::setView(int id, const ViewSettings& vs) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->view = vs;
        v->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::setHudImage(int id, int w, int h, std::vector<uint8_t> bgra) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->hud = std::move(bgra);
        v->hudW = w;
        v->hudH = h;
        v->hudDirty = true;
        if (!playing_) v->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::setHintImage(int id, int w, int h, std::vector<uint8_t> bgra) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->hint = std::move(bgra);
        v->hintW = w;
        v->hintH = h;
        v->hintDirty = true;
        v->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::setAudioImage(int id, int w, int h, std::vector<uint8_t> bgra) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->wave = std::move(bgra);
        v->waveW = w;
        v->waveH = h;
        v->waveDirty = true;
        v->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::setPlayEnd(int id, Tick end) {
    std::lock_guard<std::mutex> l(m_);
    viewers_.at(id)->playEnd = end;
}

void PlaybackEngine::seek(int id, Tick t) {
    bool restart = false;
    double sp = 1;
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        v->pos = std::max<Tick>(0, t);
        v->dirty = true;
        if (playing_ && playViewer_ == id) {
            restart = true;
            sp = speed_;
        }
    }
    if (restart) play(id, sp);
    cv_.notify_all();
}

void PlaybackEngine::setPreviewOverride(int id, Tick t) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto& v = viewers_.at(id);
        if (v->previewOverride == t) return;
        v->previewOverride = t;
        v->dirty = true;
    }
    cv_.notify_all();
}

Tick PlaybackEngine::position(int id) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = viewers_.find(id);
    return it == viewers_.end() ? 0 : it->second->pos;
}

void PlaybackEngine::stopPlaybackLocked() {
    if (!playing_) return;
    playing_ = false;
    {
        std::lock_guard<std::mutex> ml(mixMutex_);
        session_ = 0;
        ++flushReq_;
    }
    playViewer_ = -1;
}

void PlaybackEngine::play(int id, double speed) {
    {
        std::lock_guard<std::mutex> l(m_);
        stopPlaybackLocked();
        auto it = viewers_.find(id);
        if (it == viewers_.end() || !it->second->snap) return;
        Viewer& v = *it->second;
        const Sequence& seq = v.snap->seq;
        Tick end = v.playEnd != kInvalidTick ? v.playEnd : seq.duration();
        if (speed > 0 && v.pos >= end - seq.frameDur()) v.pos = 0;  // restart from the beginning
        playStart_ = v.pos;
        playStartWallUs_ = nowUs();
        speed_ = speed;
        playViewer_ = id;
        v.resetStats = true;  // applied by the render thread (it owns those fields)
        audioClock_ = speed == 1.0 && audio_ != nullptr;
        if (audioClock_) {
            std::lock_guard<std::mutex> ml(mixMutex_);
            mixSnap_ = v.snap;
            ++flushReq_;
            int64_t s = ticksToSamples(playStart_, 48000);
            mixPos_ = s;
            sessionStartSample_ = s;
            session_ = 1;
            mixer_.resetLoudness();
        }
        playing_ = true;
    }
    mixCv_.notify_all();
    cv_.notify_all();
    if (onPlayStateChanged) onPlayStateChanged(true);
}

void PlaybackEngine::pause() {
    bool was;
    {
        std::lock_guard<std::mutex> l(m_);
        was = playing_;
        stopPlaybackLocked();
    }
    cv_.notify_all();
    if (was && onPlayStateChanged) onPlayStateChanged(false);
}

ViewerStats PlaybackEngine::stats(int id) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = viewers_.find(id);
    return it == viewers_.end() ? ViewerStats{} : it->second->stats;
}

AudioStatus PlaybackEngine::audioStatus() const {
    AudioStatus s;
    s.ok = audio_ != nullptr;
    s.backend = audioBackend_;
    if (audio_) {
        s.device = audio_->name();
        s.latencyMs = audio_->latencyMs();
        s.bufferMs = audio_->bufferMs();
        s.glitches = audio_->glitches();
    } else s.device = audioError_;
    s.underruns = underruns_;
    s.mixMaxMs = mixMaxMs_.load();
    return s;
}

std::string PlaybackEngine::decodeSummary() const {
    std::lock_guard<std::mutex> l(m_);
    int hw = 0, sw = 0;
    for (const auto& [id, v] : viewers_) {
        hw += v->stats.hwSources;
        sw += v->stats.swSources;
    }
    return std::to_string(hw) + " GPU / " + std::to_string(sw) + " CPU decoders";
}

// ---------------------------------------------------------------------------------------------
// Audio

void PlaybackEngine::audioPull(float* out, int frames) {
    const int64_t base = written_.fetch_add(frames) + (audio_ ? audio_->prefillFrames() : 0);
    if (flushReq_.load() != flushAck_.load()) {
        ring_.discard();
        flushAck_ = flushReq_.load();
    }
    int s = session_.load();
    if (s == 1 && ring_.available() >= size_t(kStartThreshold)) {
        sessionDeviceIndex_ = base;
        session_ = 2;
        s = 2;
    }
    if (s == 2) {
        size_t got = ring_.read(out, size_t(frames));
        if (got < size_t(frames)) {
            std::fill(out + got * 2, out + size_t(frames) * 2, 0.f);
            // Keep A/V locked: the clock does not advance over the gap.
            sessionDeviceIndex_ += int64_t(frames) - int64_t(got);
            ++underruns_;
        }
        return;
    }
    if (s == 3) {  // scrub snippet: play what is buffered, then go idle
        size_t got = ring_.read(out, size_t(frames));
        std::fill(out + got * 2, out + size_t(frames) * 2, 0.f);
        if (ring_.available() == 0) {
            int expected = 3;
            session_.compare_exchange_strong(expected, 0);
        }
        return;
    }
    std::fill(out, out + size_t(frames) * 2, 0.f);
}

void PlaybackEngine::scrubAudio(int viewer, Tick t) {
    if (playing_ || !audio_) return;
    SnapshotPtr snap;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = viewers_.find(viewer);
        if (it == viewers_.end()) return;
        snap = it->second->snap;
    }
    {
        std::lock_guard<std::mutex> l(mixMutex_);
        scrubSnap_ = snap;
        scrubT_ = t;
        ++scrubGen_;
    }
    mixCv_.notify_all();
}

void PlaybackEngine::renderScrubSnippet(std::unique_lock<std::mutex>& l) {
    const uint64_t gen = scrubGen_;
    SnapshotPtr snap = scrubSnap_;
    const Tick t = scrubT_;
    scrubDone_ = gen;
    if (!snap) return;
    // stop reading, drop old content, wait for the audio thread to acknowledge (lock released)
    session_ = 0;
    ++flushReq_;
    for (int i = 0; i < 40 && flushAck_.load() != flushReq_.load(); ++i) {
        l.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        l.lock();
        if (quit_ || playing_ || session_.load() != 0) return;  // playback started meanwhile
    }
    if (flushAck_.load() != flushReq_.load()) return;
    constexpr int kSnippet = 2880;  // 60 ms
    std::vector<float> buf(size_t(kSnippet) * 2);
    mixer_.render(*snap, ticksToSamples(t, 48000), buf.data(), kSnippet);
    for (int i = 0; i < kSnippet; ++i) {  // short fades so snippets do not click
        float g = std::min(1.f, std::min(float(i) / 96.f, float(kSnippet - 1 - i) / 480.f));
        buf[size_t(i) * 2] *= g;
        buf[size_t(i) * 2 + 1] *= g;
    }
    ring_.write(buf.data(), kSnippet);
    session_ = 3;
}

void PlaybackEngine::mixerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::vector<float> buf(size_t(kMixBlock) * 2);
    uint64_t lastSessionFlush = 0;
    while (true) {
        std::unique_lock<std::mutex> l(mixMutex_);
        mixCv_.wait_for(l, std::chrono::milliseconds(3), [&] { return quit_; });
        if (quit_) return;
        const int sess = session_.load();
        if (sess == 0 || sess == 3) {
            if (scrubGen_ != scrubDone_ && !playing_) {
                renderScrubSnippet(l);
                continue;
            }
            l.unlock();
            mixer_.prune();
            continue;
        }
        // a new session: wait for the audio thread to drop the old ring content first
        if (flushAck_.load() != flushReq_.load()) continue;
        if (lastSessionFlush != flushReq_.load()) {
            lastSessionFlush = flushReq_.load();
            mixer_.reset();
        }
        SnapshotPtr snap = mixSnap_;
        if (!snap) continue;
        // Holding mixMutex_ while writing keeps play()/pause() from interleaving a new session.
        int guard = 0;
        while (ring_.available() < size_t(kRingTarget) && session_.load() != 0 && guard++ < 16) {
            const auto m0 = std::chrono::steady_clock::now();
            mixer_.render(*snap, mixPos_.load(), buf.data(), kMixBlock);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();
            if (ms > mixMaxMs_.load()) mixMaxMs_ = ms;
            if (ms > 15 && std::getenv("CUTLINE_MIX_LOG")) std::fprintf(stderr, "mix block at %.3f s took %.1f ms\n", mixPos_.load() / 48000.0, ms);
            ring_.write(buf.data(), kMixBlock);
            mixPos_ += kMixBlock;
        }
    }
}

bool PlaybackEngine::clockSample(int64_t& wallUs, Tick& t) {
    if (!playing_.load() || !audioClock_.load() || session_.load() != 2) return false;
    const int64_t a = nowUs();
    t = clockTime();
    wallUs = (a + nowUs()) / 2;
    return true;
}

Tick PlaybackEngine::clockTime() {
    if (audioClock_.load()) {
        if (session_.load() != 2) return playStart_;  // pre-roll: hold the first frame
        int64_t heard = audio_->playedFrames();
        int64_t rel = std::max<int64_t>(0, heard - sessionDeviceIndex_.load());
        return samplesToTicks(sessionStartSample_.load() + rel, 48000);
    }
    double us = double(nowUs() - playStartWallUs_);
    return playStart_ + Tick(us * speed_.load() * double(kTicksPerSecond) / 1e6);
}

// ---------------------------------------------------------------------------------------------
// Rendering

void PlaybackEngine::prefetch(Viewer& v, Tick t) {
    if (!v.rsnap) return;
    const RenderSnapshot& s = *v.rsnap;
    Provider p(*this, v, s, true);
    const Tick horizon = t + kTicksPerSecond * 3 / 2;
    for (const auto& c : s.seq.clips) {
        if (!c.enabled || c.kind != ClipKind::Media || c.start <= t || c.start > horizon) continue;
        const Track* tr = s.seq.track(c.trackId);
        if (!tr || tr->kind != TrackKind::Video || !tr->visible) continue;
        const AssetRef* a = s.asset(c.assetId);
        if (!a || a->offline || !a->info.hasVideo) continue;
        if (continuedFrom(s.seq, c)) continue;  // it inherits the previous piece's decoder at the cut
        p.source(c, *a)->request(c.sourceAt(c.start, s.seq.frameDur()), 6);
    }
}

void PlaybackEngine::applyBudget() {
    // Every open source gets the same window length, chosen so that all windows together
    // (sum of one frame per source x window) stay within the budget.
    double bytesPerStep = 0;
    for (const auto& [k, s] : sources_) bytesPerStep += double(s->frameBytesEstimate());
    if (bytesPerStep <= 0) return;
    const int window = int(std::clamp(double(frameBudget_.load()) / bytesPerStep, 8.0, 48.0));
    for (const auto& [k, s] : sources_) s->setWindow(window);
}

void PlaybackEngine::pruneSources(bool force) {
    const int64_t now = nowMs();
    // a source whose clip is already behind the playhead (or far ahead of it) goes as soon as it
    // is idle for a moment: short pieces (speed ramps, many cuts) would otherwise keep decoded
    // windows alive for seconds
    auto outOfReach = [&](const SourceKey& k) {
        auto vit = viewers_.find(k.viewer);
        if (vit == viewers_.end() || !vit->second->rsnap) return false;
        const Viewer& v = *vit->second;
        const Clip* c = v.rsnap->seq.clip(k.clip);  // (keys inside nests are not found: time-based only)
        if (!c) return false;
        const Tick pos = v.rstats.position;
        return c->end() + kTicksPerSecond < pos || c->start > pos + 4 * kTicksPerSecond;
    };
    for (auto it = sources_.begin(); it != sources_.end();) {
        const int64_t idleMs = now - it->second->lastUsedMs.load();
        if (force || idleMs > 4000 || (idleMs > 500 && outOfReach(it->first))) {
            held_.erase({it->first.viewer, it->first.clip});
            it = sources_.erase(it);
        } else ++it;
    }
    applyBudget();
}

bool PlaybackEngine::renderViewer(Viewer& v, Tick t, bool playing) {
    if (!v.rsnap || !v.sc.valid() || !v.sc.rtv()) return false;
    if (!v.timerInit) {
        v.timer.init(gpu_->device());
        v.timerInit = true;
    }
    const RenderSnapshot& snap = *v.rsnap;
    const ViewSettings& view = v.rview;
    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
    int div = view.quality == PreviewQuality::Auto ? v.autoDiv : int(view.quality);
    const int cw = std::max(2, W / div), ch = std::max(2, H / div);
    if (!v.canvas || v.canvas->width != cw || v.canvas->height != ch) v.canvas = comp_->makeCanvas(cw, ch, false);
    if (!v.canvas) return false;
    auto t0 = std::chrono::steady_clock::now();
    ID3D11DeviceContext* ctx = gpu_->context();
    v.timer.begin(ctx);
    Provider prov(*this, v, snap, playing);
    RenderStats st = renderer_->render(snap, t, *v.canvas, prov, true);

    // --- present ---------------------------------------------------------------------------
    const int bw = v.sc.width(), bh = v.sc.height();
    double s = view.zoom > 0 ? view.zoom : std::min(double(bw) / W, double(bh) / H);
    const double dw = W * s, dh = H * s;
    const double x0 = (bw - dw) / 2 + (view.zoom > 0 ? view.panX : 0);
    const double y0 = (bh - dh) / 2 + (view.zoom > 0 ? view.panY : 0);
    const float clearCol[4] = {0.075f, 0.075f, 0.08f, 1};
    const float black[4] = {0, 0, 0, 1};
    const float dst[4] = {float(x0), float(y0), float(x0 + dw), float(y0 + dh)};
    const bool emptyContent = snap.seq.clips.empty() && snap.seq.captions.empty();
    if (emptyContent) {
        gpu_->context()->ClearRenderTargetView(v.sc.rtv(), clearCol);
        if (v.rhintDirty && v.rhintW > 0) {
            v.hintTex = comp_->uploadBGRA(v.rhintW, v.rhintH, v.rhint.data(), v.rhintW * 4);
            v.rhintDirty = false;
        }
        if (v.hintTex) {
            const float hx = float((bw - v.hintTex->width) / 2), hy = float((bh - v.hintTex->height) / 2);
            const float r[4] = {hx, hy, hx + float(v.hintTex->width), hy + float(v.hintTex->height)};
            const float none[4] = {0, 0, 0, 0};
            comp_->blit(v.sc.rtv(), bw, bh, *v.hintTex, r, none, false, none);
        }
    } else {
        comp_->blit(v.sc.rtv(), bw, bh, *v.canvas, dst, black, true, clearCol);
        if (v.scopes.load(std::memory_order_relaxed)) captureScope(v, W, H, playing);
    }
    std::vector<OverlayQuad> quads;
    // audio-only content (e.g. a music file in the Source monitor): its waveform and a playhead
    if (v.rwaveDirty) {
        v.waveTex = v.rwaveW > 0 ? comp_->uploadBGRA(v.rwaveW, v.rwaveH, v.rwave.data(), v.rwaveW * 4) : nullptr;
        v.rwaveDirty = false;
    }
    if (!emptyContent && v.waveTex) {
        bool hasVideo = !snap.seq.captions.empty();
        for (const auto& c : snap.seq.clips)
            if (const Track* tr = snap.seq.track(c.trackId); tr && tr->kind == TrackKind::Video) hasVideo = true;
        const Tick dur = snap.seq.duration();
        if (!hasVideo && dur > 0) {
            gpu_->context()->ClearRenderTargetView(v.sc.rtv(), clearCol);
            const float m = 24, top = float(bh) * 0.2f, bottom = float(bh) * 0.8f;
            const float r[4] = {m, top, float(bw) - m, bottom};
            const float none[4] = {0, 0, 0, 0};
            comp_->blit(v.sc.rtv(), bw, bh, *v.waveTex, r, none, false, none);
            const float px = m + (float(bw) - 2 * m) * float(std::clamp<double>(double(t) / double(dur), 0, 1));
            const float pc[4] = {0.30f, 0.62f, 1.0f, 1.0f};
            quads.push_back(OverlayQuad::line(px, top - 8, px, bottom + 8, 1.5f, pc));
        }
    }
    auto map = [&](double x, double y, float& ox, float& oy) {
        ox = float(x0 + x * s);
        oy = float(y0 + y * s);
    };
    if (view.safeMargins) {
        const float c[4] = {1, 1, 1, 0.45f};
        for (double inset : {0.05, 0.10}) {
            float ax, ay, bx, by;
            map(W * inset, H * inset, ax, ay);
            map(W * (1 - inset), H * (1 - inset), bx, by);
            quads.push_back(OverlayQuad::line(ax, ay, bx, ay, 1, c));
            quads.push_back(OverlayQuad::line(bx, ay, bx, by, 1, c));
            quads.push_back(OverlayQuad::line(bx, by, ax, by, 1, c));
            quads.push_back(OverlayQuad::line(ax, by, ax, ay, 1, c));
        }
    }
    if (view.handles) {
        const float c[4] = {0.35f, 0.65f, 1.0f, 0.95f};
        float p[4][2];
        for (int i = 0; i < 4; ++i) map(view.handleCorners.p[i][0], view.handleCorners.p[i][1], p[i][0], p[i][1]);
        const int order[5] = {0, 1, 3, 2, 0};
        for (int i = 0; i < 4; ++i)
            quads.push_back(OverlayQuad::line(p[order[i]][0], p[order[i]][1], p[order[i + 1]][0], p[order[i + 1]][1], 1.5f, c));
        auto handle = [&](float x, float y) { quads.push_back(OverlayQuad::rect(x - 4, y - 4, x + 4, y + 4, c)); };
        for (int i = 0; i < 4; ++i) handle(p[i][0], p[i][1]);
        for (int i = 0; i < 4; ++i)
            handle((p[order[i]][0] + p[order[i + 1]][0]) / 2, (p[order[i]][1] + p[order[i + 1]][1]) / 2);
        float ax, ay;
        map(view.anchorX, view.anchorY, ax, ay);
        quads.push_back(OverlayQuad::line(ax - 8, ay, ax + 8, ay, 1.5f, c));
        quads.push_back(OverlayQuad::line(ax, ay - 8, ax, ay + 8, 1.5f, c));
    }
    comp_->drawQuads(v.sc.rtv(), bw, bh, quads);
    if (view.showHud) {
        if (v.rhudDirty && v.rhudW > 0) {
            v.hudTex = comp_->uploadBGRA(v.rhudW, v.rhudH, v.rhud.data(), v.rhudW * 4);
            v.rhudDirty = false;
        }
        if (v.hudTex) {
            const float r[4] = {8, 8, float(8 + v.hudTex->width), float(8 + v.hudTex->height)};
            const float none[4] = {0, 0, 0, 0};
            comp_->blit(v.sc.rtv(), bw, bh, *v.hudTex, r, none, false, none);
        }
    }
    v.timer.end(ctx);
    HRESULT hr = v.sc.present(playing);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (onError) onError("The GPU device was reset (" + gpu_->removedReason() + "). The preview will restart.");
        return false;
    }
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    double g = v.timer.poll(ctx);
    // stats
    v.rstats.renderMs = v.rstats.renderMs * 0.9 + cpuMs * 0.1;
    if (g >= 0) v.rstats.gpuMs = v.rstats.gpuMs * 0.9 + g * 0.1;
    v.rstats.previewDivisor = div;
    v.rstats.sources = v.rstats.hwSources = v.rstats.swSources = 0;
    for (const auto& [k, src] : sources_) {
        if (k.viewer != v.id || !src->ready()) continue;
        ++v.rstats.sources;
        (src->isHw() ? v.rstats.hwSources : v.rstats.swSources)++;
        v.rstats.lastSeekMs = std::max(v.rstats.lastSeekMs * 0.9, src->lastSeekMs());
    }
    v.rstats.position = t;
    ++v.rstats.framesShown;
    if (st.late || st.missing) ++v.rstats.lateFrames;
    const int64_t now = nowUs();
    v.shownAt.push_back(now);
    while (!v.shownAt.empty() && now - v.shownAt.front() > 1'000'000) v.shownAt.pop_front();
    v.rstats.fps = double(v.shownAt.size());
    v.incomplete = st.late > 0 || st.missing > 0;
    if (v.incomplete && !v.incompleteSinceMs) v.incompleteSinceMs = nowMs();
    if (!v.incomplete) v.incompleteSinceMs = 0;
    {
        std::lock_guard<std::mutex> l(m_);
        v.stats = v.rstats;
    }
    // auto quality (render cost only; decode lateness is not helped by a smaller canvas)
    if (playing && view.quality == PreviewQuality::Auto) {
        const double budget = 1000.0 / snap.seq.settings.fps.toDouble();
        const double cost = std::max(cpuMs, v.rstats.gpuMs);
        if (cost > budget * 0.85) {
            v.fast = 0;
            if (++v.slow >= 8 && v.autoDiv < 4) {
                v.autoDiv *= 2;
                v.slow = 0;
            }
        } else if (cost < budget * 0.35) {
            v.slow = 0;
            if (++v.fast >= 180 && v.autoDiv > 1) {
                v.autoDiv /= 2;
                v.fast = 0;
            }
        }
    }
    return true;
}

void PlaybackEngine::captureScope(Viewer& v, int W, int H, bool playing) {
    ID3D11DeviceContext* ctx = gpu_->context();
    const int64_t now = nowMs();
    // deliver the previous capture once the GPU is done with it (never stall playback)
    auto deliver = [&](bool wait) {
        if (!v.scopePending) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(v.scopeStaging.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
        const int w = v.scopeRT->width, h = v.scopeRT->height;
        std::vector<uint8_t> px(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(px.data() + size_t(y) * w * 4, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 4);
        ctx->Unmap(v.scopeStaging.Get(), 0);
        v.scopePending = false;
        if (onScope) onScope(v.id, w, h, std::move(px));
    };
    deliver(false);
    if (playing && now - v.lastScopeMs < 66) return;
    if (v.scopePending) return;  // GPU still busy with the last one
    const int sw = std::min(W, kScopeWidth), sh = std::max(2, int(int64_t(H) * sw / std::max(1, W)));
    if (!v.scopeRT || v.scopeRT->width != sw || v.scopeRT->height != sh) {
        v.scopeRT = gpu_->acquire(sw, sh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(sw);
        d.Height = UINT(sh);
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        v.scopeStaging.Reset();
        if (!v.scopeRT || FAILED(gpu_->device()->CreateTexture2D(&d, nullptr, &v.scopeStaging))) {
            v.scopeRT.reset();
            return;
        }
    }
    const float black[4] = {0, 0, 0, 1};
    const float r[4] = {0, 0, float(sw), float(sh)};
    comp_->blit(v.scopeRT->rtv0.Get(), sw, sh, *v.canvas, r, black, true, black);
    ctx->CopyResource(v.scopeStaging.Get(), v.scopeRT->tex.Get());
    v.scopePending = true;
    v.lastScopeMs = now;
    if (!playing) deliver(true);  // paused: a single frame, show it now
}

void PlaybackEngine::setScopes(int id, bool on) {
    std::lock_guard<std::mutex> l(m_);
    if (auto it = viewers_.find(id); it != viewers_.end()) {
        it->second->scopes = on;
        it->second->dirty = true;
    }
    cv_.notify_all();
}

void PlaybackEngine::renderLoop() {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    int64_t lastPrune = nowMs();
    for (;;) {
        std::vector<Viewer*> todo;
        Viewer* pv = nullptr;
        bool playing;
        {
            std::unique_lock<std::mutex> l(m_);
            // window ops first (swapchains live on this thread)
            for (auto& [id, v] : viewers_) {
                if (!v->windowDirty) continue;
                if (!v->hwnd) {
                    v->sc.destroy();
                } else if (!v->sc.valid() || v->sc.hwnd() != v->hwnd) {
                    std::string err;
                    v->sc.destroy();
                    if (!v->sc.create(*gpu_, v->hwnd, v->w, v->h, err) && onError) onError(err);
                } else {
                    v->sc.resize(v->w, v->h);
                }
                v->windowDirty = false;
            }
            if (quit_) break;
            playing = playing_;
            const int64_t now = nowMs();
            for (auto& [id, v] : viewers_) {
                const bool poll = v->incomplete && v->incompleteSinceMs && now - v->incompleteSinceMs < 10000;
                if (playing && id == playViewer_) pv = v.get();
                else if (v->dirty || poll) todo.push_back(v.get());
            }
            if (!playing && todo.empty()) {
                cv_.wait_for(l, std::chrono::milliseconds(200));
                continue;
            }
            for (Viewer* v : todo) v->dirty = false;
            auto take = [](Viewer* v) {
                if (v->resetStats) {
                    v->rstats = {};
                    v->shownAt.clear();
                    v->lastRendered = kInvalidTick;
                    v->resetStats = false;
                }
                v->rsnap = v->snap;
                v->rview = v->view;
                if (v->waveDirty) {
                    v->rwave = v->wave;
                    v->rwaveW = v->waveW;
                    v->rwaveH = v->waveH;
                    v->rwaveDirty = true;
                    v->waveDirty = false;
                }
                if (v->hintDirty) {
                    v->rhint = v->hint;
                    v->rhintW = v->hintW;
                    v->rhintH = v->hintH;
                    v->rhintDirty = true;
                    v->hintDirty = false;
                }
                if (v->hudDirty) {
                    v->rhud = v->hud;
                    v->rhudW = v->hudW;
                    v->rhudH = v->hudH;
                    v->rhudDirty = true;
                    v->hudDirty = false;
                }
            };
            if (pv) take(pv);
            for (Viewer* v : todo) take(v);
        }

        if (pv) {
            Tick t = clockTime();
            const RenderSnapshot* snap = pv->rsnap.get();
            const Rational fps = snap->seq.settings.fps;
            const Tick fd = snap->seq.frameDur();
            Tick ft = floorToFrame(std::max<Tick>(0, t), fps);
            Tick end;
            {
                std::lock_guard<std::mutex> l(m_);
                end = pv->playEnd != kInvalidTick ? pv->playEnd : snap->seq.duration();
            }
            const double sp = speed_.load();
            bool stop = false;
            if (sp > 0 && ft >= end) {
                ft = std::max<Tick>(0, end - fd);
                stop = true;
            } else if (sp < 0 && t <= 0) {
                ft = 0;
                stop = true;
            }
            if (ft != pv->lastRendered) {
                if (pv->lastRendered != kInvalidTick) {
                    int64_t step = std::llabs(ft - pv->lastRendered) / std::max<Tick>(1, fd);
                    const int64_t expected = std::max<int64_t>(1, int64_t(std::llround(std::abs(sp))));
                    if (step > expected) pv->rstats.dropped += uint64_t(step - expected);
                }
                renderViewer(*pv, ft, true);
                pv->lastRendered = ft;
                {
                    std::lock_guard<std::mutex> l(m_);
                    pv->pos = ft;
                }
                if (onPosition) onPosition(pv->id, ft);
                prefetch(*pv, ft);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (stop) {
                pause();
                if (onStopped) onStopped(pv->id);
            }
        }
        for (Viewer* v : todo) {
            Tick pos;
            {
                std::lock_guard<std::mutex> l(m_);
                pos = v->previewOverride != kInvalidTick ? v->previewOverride : v->pos;
            }
            if (v->rsnap) pos = std::clamp<Tick>(pos, 0, std::max<Tick>(0, v->rsnap->seq.duration()));
            const bool wasIncomplete = v->incomplete;
            renderViewer(*v, pos, false);
            v->lastRendered = pos;
            if (wasIncomplete && v->incomplete) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (nowMs() - lastPrune > 500) {
            pruneSources(false);
            lastPrune = nowMs();
        }
    }
    sources_.clear();
    held_.clear();
    for (auto& [id, v] : viewers_) {
        v->canvas.reset();
        v->hudTex.reset();
        v->sc.destroy();
    }
    renderer_.reset();
    timeEndPeriod(1);
}

} // namespace cut
