#include "engine/media_cache.h"

#include "core/project_io.h"
#include "media/frame_reader.h"
#include "media/thumbnailer.h"

#include <QPainter>

#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cut {

namespace {
fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string str(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
} // namespace

std::string appDataDir() {
    std::string out;
    if (const char* o = std::getenv("CUTLINE_APPDATA")) {  // tests: isolated settings/autosave/logs
        std::error_code ec;
        fs::create_directories(u8(o), ec);
        return o;
    }
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        out = str(fs::path(p) / "Cutline");
        CoTaskMemFree(p);
    } else out = "Cutline";
    std::error_code ec;
    fs::create_directories(u8(out), ec);
    return out;
}

std::string defaultCacheDir() { return str(u8(appDataDir()) / "cache"); }

int ThumbStrip::indexAt(Tick t) const {
    if (times.empty()) return -1;
    auto it = std::upper_bound(times.begin(), times.end(), t);
    if (it == times.begin()) return 0;
    int i = int(it - times.begin()) - 1;
    if (size_t(i + 1) < times.size() && times[size_t(i + 1)] - t < t - times[size_t(i)]) ++i;
    return i;
}

bool Waveform::range(int64_t s0, int64_t s1, float& mn, float& mx) const {
    const size_t rd = ready.load();
    if (rd == 0 || s1 <= s0) return false;
    int64_t span = (s1 - s0) / kSamplesPerPeak;
    // pick the coarsest level with at least ~1 pair in the range
    const std::vector<int8_t>* data = &peaks;
    int64_t per = kSamplesPerPeak;
    size_t avail = rd;
    if (complete.load()) {
        int64_t p = kSamplesPerPeak;
        for (const auto& l : levels) {
            if (span / 4 < 1) break;
            p *= 4;
            span /= 4;
            data = &l;
            per = p;
            avail = l.size() / 2;
        }
    }
    int64_t a = std::max<int64_t>(0, s0 / per), b = std::min<int64_t>(int64_t(avail), (s1 + per - 1) / per);
    if (b <= a) return false;
    int lo = 127, hi = -127;
    for (int64_t i = a; i < b; ++i) {
        lo = std::min<int>(lo, (*data)[size_t(i) * 2]);
        hi = std::max<int>(hi, (*data)[size_t(i) * 2 + 1]);
    }
    mn = lo / 127.f;
    mx = hi / 127.f;
    return true;
}

void Waveform::buildLevels() {
    levels.clear();
    const std::vector<int8_t>* src = &peaks;
    size_t n = ready.load();
    while (n > 4) {
        std::vector<int8_t> l((n + 3) / 4 * 2);
        for (size_t i = 0; i < (n + 3) / 4; ++i) {
            int lo = 127, hi = -127;
            for (size_t k = i * 4; k < std::min(n, i * 4 + 4); ++k) {
                lo = std::min<int>(lo, (*src)[k * 2]);
                hi = std::max<int>(hi, (*src)[k * 2 + 1]);
            }
            l[i * 2] = int8_t(lo);
            l[i * 2 + 1] = int8_t(hi);
        }
        n = l.size() / 2;
        levels.push_back(std::move(l));
        src = &levels.back();
        if (levels.size() >= 6) break;
    }
}

MediaCache::MediaCache() { setDir({}); }

void MediaCache::setDir(const std::string& d) {
    std::lock_guard<std::mutex> l(m_);
    dir_ = d.empty() ? defaultCacheDir() : d;
    std::error_code ec;
    fs::create_directories(u8(dir_), ec);
}

std::string MediaCache::dir() const {
    std::lock_guard<std::mutex> l(m_);
    return dir_;
}

std::string MediaCache::keyFor(const Asset& a) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    std::string p = a.path;
    for (char& c : p) c = char(std::tolower((unsigned char)c));
    mix(p.data(), p.size());
    mix(&a.info.fileSize, sizeof(a.info.fileSize));
    mix(&a.info.mtime, sizeof(a.info.mtime));
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

std::string MediaCache::proxyPath(const Asset& a) const { return str(u8(dir()) / (keyFor(a) + "_proxy.mp4")); }
std::string MediaCache::thumbsPath(const Asset& a) const { return str(u8(dir()) / (keyFor(a) + "_thumbs.png")); }
std::string MediaCache::peaksPath(const Asset& a, int stream) const {
    return str(u8(dir()) / (keyFor(a) + "_a" + std::to_string(stream) + ".peaks"));
}

bool MediaCache::proxyReady(const Asset& a) const {
    std::error_code ec;
    auto p = u8(proxyPath(a));
    return fs::exists(p, ec) && fs::file_size(p, ec) > 1024;
}

ThumbStripPtr MediaCache::loadThumbs(const Asset& a) const {
    std::string path = thumbsPath(a);
    std::string meta;
    if (!readFileUtf8(path + ".txt", meta)) return nullptr;
    QImage img;
    if (!img.load(QString::fromStdString(path))) return nullptr;
    auto s = std::make_shared<ThumbStrip>();
    std::istringstream in(meta);
    size_t n = 0;
    in >> s->w >> s->h >> n;
    for (size_t i = 0; i < n; ++i) {
        Tick t = 0;
        in >> t;
        s->times.push_back(t);
    }
    if (!s->w || s->times.empty()) return nullptr;
    s->strip = img.convertToFormat(QImage::Format_RGB32);
    touch(path);
    return s;
}

bool MediaCache::saveThumbs(const Asset& a, const ThumbStrip& s) const {
    std::string path = thumbsPath(a);
    std::string meta = std::to_string(s.w) + " " + std::to_string(s.h) + " " + std::to_string(s.times.size());
    for (Tick t : s.times) meta += " " + std::to_string(t);
    return s.strip.save(QString::fromStdString(path), "PNG") && writeFileAtomic(path + ".txt", meta);
}

WaveformPtr MediaCache::loadPeaks(const Asset& a, int stream) const {
    std::string data;
    std::string path = peaksPath(a, stream);
    if (!readFileUtf8(path, data) || data.size() < 8 || data.compare(0, 4, "CPK1") != 0) return nullptr;
    auto w = std::make_shared<Waveform>();
    w->peaks.assign(data.begin() + 8, data.end());
    w->ready = w->peaks.size() / 2;
    w->buildLevels();
    w->complete = true;
    touch(path);
    return w;
}

bool MediaCache::savePeaks(const Asset& a, int stream, const Waveform& w) const {
    std::string data = "CPK1";
    uint32_t spp = Waveform::kSamplesPerPeak;
    data.append(reinterpret_cast<const char*>(&spp), 4);
    data.append(reinterpret_cast<const char*>(w.peaks.data()), w.ready.load() * 2);
    return writeFileAtomic(peaksPath(a, stream), data);
}

ThumbStripPtr MediaCache::generateThumbs(const Asset& a, JobToken& tok) {
    if (!a.info.hasVideo) return nullptr;
    const int th = 72;
    Tick dur = a.info.duration;
    Tick interval = std::max<Tick>(kTicksPerSecond, dur / 300);
    std::vector<QImage> imgs;
    std::vector<Tick> times;
    int w = 0;
    bool ok = extractThumbnails(
        a.path, a.info, th, interval,
        [&](Tick t, const uint8_t* bgra, int iw, int ih, int pitch) {
            QImage img(bgra, iw, ih, pitch, QImage::Format_ARGB32);
            imgs.push_back(img.copy());
            times.push_back(t);
            w = iw;
            if (dur > 0) tok.progress = double(t) / double(dur);
        },
        [&] { return !tok.checkpoint(); });
    if (!ok || imgs.empty()) return nullptr;
    auto s = std::make_shared<ThumbStrip>();
    s->w = w;
    s->h = th;
    s->times = times;
    s->strip = QImage(w * int(imgs.size()), th, QImage::Format_RGB32);
    QPainter p(&s->strip);
    for (size_t i = 0; i < imgs.size(); ++i) p.drawImage(int(i) * w, 0, imgs[i]);
    p.end();
    return s;
}

bool MediaCache::generatePeaks(const Asset& a, int stream, Waveform& w, JobToken& tok) {
    if (stream < 0 || size_t(stream) >= a.info.audio.size()) return false;
    AudioReader r;
    std::string err;
    if (!r.open(a.path, a.info.audio[size_t(stream)].index, a.info.origin, err)) return false;
    const int64_t total = std::max<int64_t>(r.lengthSamples(), ticksToSamples(a.info.duration, 48000));
    const size_t pairs = size_t(total / Waveform::kSamplesPerPeak + 1);
    w.peaks.assign(pairs * 2, 0);
    const int block = Waveform::kSamplesPerPeak * 64;
    std::vector<float> buf(size_t(block) * 2);
    size_t pi = 0;
    for (int64_t pos = 0; pos < total && pi < pairs; pos += block) {
        if (!tok.checkpoint()) return false;
        r.read(pos, buf.data(), block);
        for (int k = 0; k < 64 && pi < pairs; ++k, ++pi) {
            float lo = 0, hi = 0;
            for (int i = 0; i < Waveform::kSamplesPerPeak; ++i) {
                size_t o = size_t(k * Waveform::kSamplesPerPeak + i) * 2;
                float v = (buf[o] + buf[o + 1]) * 0.5f;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            w.peaks[pi * 2] = int8_t(std::clamp(std::lround(lo * 127), -127L, 127L));
            w.peaks[pi * 2 + 1] = int8_t(std::clamp(std::lround(hi * 127), -127L, 127L));
        }
        w.ready = pi;
        tok.progress = double(pos) / double(total);
    }
    w.ready = pi;
    w.buildLevels();
    w.complete = true;
    return true;
}

uint64_t MediaCache::usageBytes() const {
    uint64_t total = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(dir()), ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}

void MediaCache::touch(const std::string& path) const {
    std::error_code ec;
    fs::last_write_time(u8(path), fs::file_time_type::clock::now(), ec);
}

void MediaCache::purge(const std::set<std::string>& inUse) {
    struct F {
        fs::path p;
        uint64_t size;
        fs::file_time_type t;
    };
    std::vector<F> files;
    uint64_t total = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(dir()), ec)) {
        if (!e.is_regular_file(ec)) continue;
        files.push_back({e.path(), e.file_size(ec), e.last_write_time(ec)});
        total += files.back().size;
    }
    if (total <= cap_) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
    for (const auto& f : files) {
        if (total <= cap_) break;
        if (inUse.count(str(f.p))) continue;
        fs::remove(f.p, ec);
        if (!ec) total -= f.size;
    }
}

void MediaCache::clearAll() {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(dir()), ec)) fs::remove(e.path(), ec);
}

} // namespace cut
