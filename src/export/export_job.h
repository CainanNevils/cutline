#pragma once
// Export: runs on its own threads with its own GPU device (the UI and preview stay responsive).
// Always renders from the original media, never proxies. Frame-exact: N timeline frames in the
// range produce exactly N video frames; audio covers exactly the same duration.
#include "engine/snapshot.h"
#include "export/encoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cut {

struct ExportSettings {
    std::string path;
    int width = 2560, height = 1440;
    Rational fps{0, 1};  // 0 = sequence frame rate
    Tick rangeStart = 0, rangeEnd = 0;  // timeline range [start, end)
    VideoEncodeConfig video;
    bool audio = true;
    int audioBitrate = 192000;
    // audio only (no picture, no GPU): .wav / .mp3 / .flac / .m4a with this encoder
    bool withVideo = true;
    std::string audioCodec = "aac";
    // Loudness normalization: 0 = off, else the integrated loudness target (LUFS, e.g. -14).
    // A pre-pass measures the mix; the gain is limited so sample peaks stay below -1 dBFS.
    double normalizeLufs = 0;
    bool burnCaptions = false;
    bool sidecarSrt = false;
    bool highQuality = false;  // RGBA16F compositing
    bool allowHwDecode = true;
    bool allowFallback = true;  // NVENC -> libx264 when the GPU encoder cannot open
};

struct ExportStatus {
    std::atomic<int64_t> framesDone{0};
    int64_t framesTotal = 0;
    std::atomic<double> fps{0};
    std::atomic<double> analyzeProgress{-1};  // loudness pre-pass 0..1 (-1 = none)
    std::atomic<bool> finished{false}, failed{false}, cancelled{false};
    std::mutex m;
    std::string error, videoPath, encoder, decodeInfo;
    double measuredLufs = 0, appliedGainDb = 0;  // loudness normalization (when enabled)
    double seconds = 0;
};

class ExportJob {
public:
    ExportJob(SnapshotPtr snap, ExportSettings s);
    ~ExportJob();
    void start();
    void cancel() { cancel_ = true; }
    void wait();
    bool running() const { return running_.load(); }
    ExportStatus& status() { return st_; }
    const ExportSettings& settings() const { return s_; }

    // Free-space check before starting (bytes needed estimate vs free).
    static bool enoughDiskSpace(const ExportSettings& s, uint64_t& need, uint64_t& free);

private:
    void run();
    bool runAudio(class Muxer& mux, int stream, class AudioEncoder& enc);
    void runAudioOnly();
    bool analyzeLoudness();  // sets gain_ for normalizeLufs
    float gain_ = 1.0f;
    SnapshotPtr snap_;
    ExportSettings s_;
    ExportStatus st_;
    std::thread thread_;
    std::atomic<bool> cancel_{false}, running_{false};
};

} // namespace cut
