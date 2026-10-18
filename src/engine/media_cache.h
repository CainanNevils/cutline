#pragma once
// Disk cache (proxies, thumbnail strips, waveform peaks) under %LOCALAPPDATA%\Cutline\cache by
// default. Entries are keyed by file path + size + mtime, so a changed source never reuses stale
// data. Total size is capped (default 20 GB) with least-recently-used purging.
#include "core/model.h"
#include "engine/job_pool.h"

#include <QImage>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace cut {

struct ThumbStrip {
    int w = 0, h = 0;
    std::vector<Tick> times;  // source time of each thumbnail
    QImage strip;             // thumbnails side by side (w * count x h)
    // Thumbnail index nearest to source time t.
    int indexAt(Tick t) const;
};
using ThumbStripPtr = std::shared_ptr<const ThumbStrip>;

// Min/max peaks of one audio stream (mono mix), 256 samples per peak at 48 kHz, with coarser
// levels. Filled progressively while generating (ready = valid pairs).
struct Waveform {
    static constexpr int kSamplesPerPeak = 256;
    std::vector<int8_t> peaks;  // pairs (min, max), scaled to +-127
    std::atomic<size_t> ready{0};
    std::vector<std::vector<int8_t>> levels;  // level i = 4^(i+1) * 256 samples per pair
    std::atomic<bool> complete{false};
    // Min/max over [s0, s1) (48 kHz samples); false if nothing ready there.
    bool range(int64_t s0, int64_t s1, float& mn, float& mx) const;
    void buildLevels();
};
using WaveformPtr = std::shared_ptr<Waveform>;

class MediaCache {
public:
    MediaCache();
    void setDir(const std::string& dir);  // empty = default
    std::string dir() const;
    void setCapBytes(uint64_t cap) { cap_ = cap; }
    uint64_t capBytes() const { return cap_; }

    static std::string keyFor(const Asset& a);
    std::string proxyPath(const Asset& a) const;
    std::string thumbsPath(const Asset& a) const;
    std::string peaksPath(const Asset& a, int stream) const;

    bool proxyReady(const Asset& a) const;
    ThumbStripPtr loadThumbs(const Asset& a) const;
    bool saveThumbs(const Asset& a, const ThumbStrip& s) const;
    WaveformPtr loadPeaks(const Asset& a, int stream) const;
    bool savePeaks(const Asset& a, int stream, const Waveform& w) const;

    // Generators (run inside jobs).
    static ThumbStripPtr generateThumbs(const Asset& a, JobToken& tok);
    static bool generatePeaks(const Asset& a, int stream, Waveform& out, JobToken& tok);

    uint64_t usageBytes() const;
    // Deletes least recently used files until under the cap; `inUse` paths are kept.
    void purge(const std::set<std::string>& inUse = {});
    void touch(const std::string& path) const;
    void clearAll();

private:
    std::string dir_;
    std::atomic<uint64_t> cap_{uint64_t(20) << 30};
    mutable std::mutex m_;
};

std::string defaultCacheDir();
std::string appDataDir();  // %LOCALAPPDATA%\Cutline

} // namespace cut
