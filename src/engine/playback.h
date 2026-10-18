#pragma once
// Playback engine: one render thread (owns the D3D11 immediate context for preview), one mixer
// thread, one audio output (master clock), and any number of viewers (Source / Program monitor).
//
// Threads and ownership:
//   UI thread    -> calls the public API (all methods are thread-safe, never block on decode)
//   render thread-> swapchains, compositor, video sources, presents
//   mixer thread -> mixes the playing viewer's audio into a lock-free ring
//   audio thread -> WASAPI pulls from the ring; its played position is the clock
#include "audio/audio_output.h"
#include "audio/mixer.h"
#include "audio/ring.h"
#include "engine/frame_renderer.h"
#include "engine/snapshot.h"
#include "engine/video_source.h"
#include "render/compositor.h"
#include "render/swapchain.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace cut {

enum class PreviewQuality { Auto = 0, Full = 1, Half = 2, Quarter = 4 };

struct ViewSettings {
    double zoom = 0;  // 0 = fit, else 1.0 = 100 %
    double panX = 0, panY = 0;  // pixels (zoomed views)
    PreviewQuality quality = PreviewQuality::Auto;
    bool safeMargins = false;
    bool showHud = false;
    // on-canvas handles for the selected clip (sequence pixel corners)
    bool handles = false;
    Corners handleCorners{};
    double anchorX = 0, anchorY = 0;  // anchor point (sequence px)
};

struct ViewerStats {
    double fps = 0;
    uint64_t framesShown = 0, dropped = 0, lateFrames = 0;
    double renderMs = 0, gpuMs = 0;
    int previewDivisor = 1;
    int sources = 0, hwSources = 0, swSources = 0;
    double lastSeekMs = 0;
    Tick position = 0;
};

struct AudioStatus {
    std::string backend, device;
    double latencyMs = 0, bufferMs = 0;
    uint64_t underruns = 0, glitches = 0;
    double mixMaxMs = 0;  // slowest 10 ms mixer block so far (> ~80 ms means an underrun)
    bool ok = false;
};

class PlaybackEngine {
public:
    PlaybackEngine();
    ~PlaybackEngine();
    // Output endpoint id for start() (empty = system default; CUTLINE_AUDIO_DEVICE always wins).
    void setAudioDevice(std::string id) { audioDevice_ = std::move(id); }
    bool start(std::string& error);
    void shutdown();
    std::shared_ptr<GpuDevice> gpu() const { return gpu_; }

    int addViewer();
    void attachWindow(int viewer, HWND hwnd, int w, int h);
    void resizeWindow(int viewer, int w, int h);
    void detachWindow(int viewer);
    void setContent(int viewer, SnapshotPtr snap);
    void setView(int viewer, const ViewSettings& v);
    void setHudImage(int viewer, int w, int h, std::vector<uint8_t> bgraPremul);
    // Centred image shown instead of the black frame while the content is empty.
    void setHintImage(int viewer, int w, int h, std::vector<uint8_t> bgraPremul);
    // Waveform image shown stretched (with a playhead) while the content has no video; w = 0 clears.
    void setAudioImage(int viewer, int w, int h, std::vector<uint8_t> bgraPremul);

    void seek(int viewer, Tick t);
    // Temporarily display 	 (e.g. the edge being trimmed) without moving the position;
    // kInvalidTick returns to the normal position.
    void setPreviewOverride(int viewer, Tick t);
    // Play ~60 ms of the viewer's audio at t (audio scrubbing while paused).
    void scrubAudio(int viewer, Tick t);
    // speed 1 = normal playback with audio; other speeds (2,4,8,-1,-2,...) shuttle without audio.
    void play(int viewer, double speed);
    void pause();
    bool isPlaying() const { return playing_.load(); }
    int playingViewer() const { return playViewer_.load(); }
    double playSpeed() const { return speed_.load(); }
    Tick position(int viewer) const;
    // (steady_clock us, timeline time heard at that moment) while playing at normal speed with the
    // audio clock running; false otherwise. Used to line recordings up with the timeline.
    bool clockSample(int64_t& wallUs, Tick& t);
    // Stop playback at `end` (exclusive) instead of the content end (in/out playback).
    void setPlayEnd(int viewer, Tick end);

    ViewerStats stats(int viewer) const;
    MeterSnapshot meters() { return mixer_.takeMeters(); }
    AudioStatus audioStatus() const;
    // GPU memory budget for decoded preview frames (default 3 GB). Each source's decode window is
    // sized so that all open sources together stay inside it.
    void setFrameBudget(size_t bytes) { frameBudget_ = bytes; }
    size_t frameBudget() const { return frameBudget_; }
    // Frames rendered from proxies are fine for preview; export always uses originals.
    std::string decodeSummary() const;

    // Called on the render thread; the UI must marshal to its own thread.
    std::function<void(int viewer, Tick t)> onPosition;
    std::function<void(int viewer)> onStopped;
    // Scope frames (RGBA8, tightly packed, <= kScopeWidth wide) from the render thread.
    std::function<void(int viewer, int w, int h, std::vector<uint8_t> rgba)> onScope;
    static constexpr int kScopeWidth = 480;
    void setScopes(int viewer, bool on);
    std::function<void(const std::string&)> onError;

    // Background-job throttling hint (true while playing).
    std::function<void(bool playing)> onPlayStateChanged;

private:
    struct Viewer;
    class Provider;
    void renderLoop();
    void mixerLoop();
    void audioPull(float* out, int frames);
    bool renderViewer(Viewer& v, Tick t, bool playing);
    void presentViewer(Viewer& v);
    void captureScope(Viewer& v, int W, int H, bool playing);
    Tick clockTime();
    void stopPlaybackLocked();
    void prefetch(Viewer& v, Tick t);
    void pruneSources(bool force);

    std::shared_ptr<GpuDevice> gpu_;
    std::unique_ptr<Compositor> comp_;
    std::unique_ptr<FrameRenderer> renderer_;
    std::unique_ptr<AudioOutput> audio_;
    std::string audioBackend_, audioError_, audioDevice_;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::map<int, std::unique_ptr<Viewer>> viewers_;
    int nextViewer_ = 0;
    bool quit_ = false;
    std::thread renderThread_, mixerThread_;

    // playback state
    std::atomic<bool> playing_{false};
    std::atomic<int> playViewer_{-1};
    std::atomic<double> speed_{1.0};
    Tick playStart_ = 0;
    int64_t playStartWallUs_ = 0;
    std::atomic<bool> audioClock_{false};

    // audio session (mixer -> ring -> device)
    Mixer mixer_;
    StereoRing ring_{4800 * 4};
    std::atomic<uint64_t> flushReq_{0}, flushAck_{0};
    std::atomic<int> session_{0};  // 0 idle, 1 starting (prefilling), 2 running, 3 scrub snippet
    SnapshotPtr scrubSnap_;          // guarded by mixMutex_
    Tick scrubT_ = 0;
    uint64_t scrubGen_ = 0, scrubDone_ = 0;
    void renderScrubSnippet(std::unique_lock<std::mutex>& l);
    std::atomic<int64_t> sessionStartSample_{0};
    std::atomic<int64_t> sessionDeviceIndex_{0};
    std::atomic<int64_t> written_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<double> mixMaxMs_{0};
    std::atomic<int64_t> mixPos_{0};
    SnapshotPtr mixSnap_;
    std::mutex mixMutex_;
    std::condition_variable mixCv_;

    // sources: key = (viewer, clip id, proxy)
    struct SourceKey {
        int viewer;
        Id clip;
        bool proxy;
        bool operator<(const SourceKey& o) const {
            return std::tie(viewer, clip, proxy) < std::tie(o.viewer, o.clip, o.proxy);
        }
    };
    std::map<SourceKey, std::shared_ptr<VideoSource>> sources_;  // render thread only
    std::map<std::pair<int, Id>, GpuFramePtr> held_;               // last frame shown per clip
    std::atomic<size_t> frameBudget_{size_t(3) << 30};
    void applyBudget();
};

} // namespace cut
