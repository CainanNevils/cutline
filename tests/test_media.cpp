// Media engine tests (need testmedia/ from scripts/gen-testmedia.ps1 and a D3D11 GPU).
#ifdef CUTLINE_HAVE_MEDIA
#include <doctest/doctest.h>

#include "audio/clip_audio.h"
#include "media/frame_reader.h"
#include "media/probe.h"
#include "media/scene_detect.h"
#include "render/compositor.h"

#include <cmath>
#include <filesystem>
#include <random>

using namespace cut;

namespace {
std::string media(const char* name) { return std::string(CUTLINE_TESTMEDIA) + "/" + name; }
bool have(const char* name) { return std::filesystem::exists(media(name)); }

struct Gpu {
    std::shared_ptr<GpuDevice> dev;
    Compositor comp;
    Gpu() {
        std::string err;
        dev = GpuDevice::create(false, err);
        if (dev) comp.init(dev, err);
    }
};
Gpu& gpu() {
    static Gpu g;
    return g;
}

// Hash of the luma in the burned-in counter region (top-left), via GPU readback.
uint64_t counterHash(const GpuFramePtr& f) {
    std::vector<uint8_t> data;
    int pitch = 0;
    if (!f || !gpu().comp.readback(*f->tex, data, pitch)) return 0;
    uint64_t h = 1469598103934665603ull;
    const int w = std::min(f->width, 420), hgt = std::min(f->height, 170);
    for (int y = 0; y < hgt; ++y)
        for (int x = 0; x < w; ++x) h = (h ^ (data[size_t(y) * pitch + x] >> 3)) * 1099511628211ull;
    return h;
}

struct Ref {
    Tick pts;
    uint64_t hash;
};

std::vector<Ref> reference(const std::string& path, const MediaInfo& mi, Tick upTo, bool hw) {
    VideoDecoder d;
    VideoDecoder::Options o;
    o.allowHw = hw;
    std::string err;
    REQUIRE(d.open(path, gpu().dev, mi.origin, o, err));
    std::vector<Ref> r;
    DecodedPicture p;
    while (d.next(INT64_MIN, p) && p.pts <= upTo) r.push_back({p.pts, counterHash(p.frame)});
    return r;
}

void checkSeeks(const char* file, bool hw, int count, Tick span) {
    if (!have(file) || !gpu().dev) return;
    std::string path = media(file);
    MediaInfo mi = probeMedia(path);
    REQUIRE(mi.valid);
    auto ref = reference(path, mi, span, hw);
    REQUIRE(ref.size() > 10);
    FrameReader rd;
    VideoDecoder::Options o;
    o.allowHw = hw;
    std::string err;
    REQUIRE(rd.open(path, gpu().dev, mi, o, err));
    const Tick tol = rd.frameDuration() / 2;
    std::mt19937 rng(7);
    std::uniform_int_distribution<Tick> dist(0, ref.back().pts);
    int wrong = 0;
    for (int i = 0; i < count; ++i) {
        Tick t = dist(rng);
        if (i % 3 == 0) rd.clearWindow();  // force real seeks, others exercise forward/window paths
        GpuFramePtr f = rd.get(t);
        REQUIRE(f);
        // expected: greatest reference pts <= t + tol
        const Ref* exp = &ref.front();
        for (const auto& r : ref)
            if (r.pts <= t + tol) exp = &r;
        if (f->pts != exp->pts || counterHash(f) != exp->hash) {
            ++wrong;
            INFO("t=" << ticksToSeconds(t) << " got pts " << ticksToSeconds(f->pts) << " want "
                      << ticksToSeconds(exp->pts));
            CHECK(f->pts == exp->pts);
        }
    }
    CHECK(wrong == 0);
    // reverse stepping (J / reversed clips) returns every frame in order
    rd.clearWindow();
    size_t start = std::min<size_t>(ref.size() - 1, 150);
    for (size_t i = start; i > start - std::min<size_t>(start, 90); --i) {
        GpuFramePtr f = rd.get(ref[i].pts);
        REQUIRE(f);
        CHECK(f->pts == ref[i].pts);
    }
}
} // namespace

TEST_CASE("probe: multi-stream, VFR, images, audio-only, garbage") {
    if (!have("multiaudio.mkv")) return;
    MediaInfo m = probeMedia(media("multiaudio.mkv"));
    REQUIRE(m.valid);
    CHECK(m.hasVideo);
    REQUIRE(m.audio.size() == 2);
    CHECK(m.audio[0].sampleRate == 48000);
    CHECK(m.audio[0].channels == 2);
    CHECK(m.audio[1].sampleRate == 44100);
    CHECK(m.audio[1].channels == 1);
    CHECK(m.audio[1].title == "mic");
    MediaInfo v = probeMedia(media("vfr_1080p.mkv"));
    CHECK(v.video.vfr);
    MediaInfo c = probeMedia(media("main_1440p60_h264.mkv"));
    CHECK_FALSE(c.video.vfr);
    CHECK(c.video.width == 2560);
    MediaInfo p = probeMedia(media("still.png"));
    CHECK(p.valid);
    CHECK(p.isImage);
    MediaInfo a = probeMedia(media("music.mp3"));
    CHECK(a.valid);
    CHECK_FALSE(a.hasVideo);
    CHECK(a.duration > 11 * kTicksPerSecond);
    MediaInfo h = probeMedia(media("hevc10_1440p60.mkv"));
    CHECK(h.video.bitDepth == 10);
    MediaInfo bad = probeMedia(std::string(CUTLINE_TESTMEDIA) + "/../CMakeLists.txt");
    CHECK_FALSE(bad.valid);
    CHECK_FALSE(bad.error.empty());
    MediaInfo none = probeMedia("Z:/does/not/exist.mp4");
    CHECK_FALSE(none.valid);
}

TEST_CASE("frame-exact seek: CFR 1440p (GPU decode)") { checkSeeks("main_1440p60_h264.mkv", true, 60, 30 * kTicksPerSecond); }
TEST_CASE("frame-exact seek: VFR (GPU decode)") { checkSeeks("vfr_1080p.mkv", true, 60, 20 * kTicksPerSecond); }
TEST_CASE("frame-exact seek: forced software decode") { checkSeeks("vfr_1080p.mkv", false, 30, 20 * kTicksPerSecond); }
TEST_CASE("frame-exact seek: HEVC with B-frames") { checkSeeks("uw_3440x1440p60_hevc.mkv", true, 30, 20 * kTicksPerSecond); }
TEST_CASE("frame-exact seek: AV1 on the GPU and in software") {
    checkSeeks("av1_1440p60.mkv", true, 30, 15 * kTicksPerSecond);
    checkSeeks("av1_1440p60.mkv", false, 15, 15 * kTicksPerSecond);
    if (!have("av1_1440p60.mkv") || !gpu().dev) return;
    // the GPU path really runs on the GPU (the default AV1 decoder, libdav1d, is CPU only)
    MediaInfo mi = probeMedia(media("av1_1440p60.mkv"));
    FrameReader rd;
    VideoDecoder::Options o;
    std::string err;
    REQUIRE(rd.open(media("av1_1440p60.mkv"), gpu().dev, mi, o, err));
    REQUIRE(rd.get(kTicksPerSecond));
    CHECK(rd.isHw());
}

TEST_CASE("audio reader: resampled mono 44.1k mic and sample-accurate reads") {
    if (!have("multiaudio.mkv")) return;
    MediaInfo m = probeMedia(media("multiaudio.mkv"));
    AudioReader a;
    std::string err;
    REQUIRE(a.open(media("multiaudio.mkv"), m.audio[1].index, m.origin, err));
    std::vector<float> buf(4800 * 2);
    a.read(48000, buf.data(), 4800);
    // 880 Hz mono tone, centred (-3 dB constant-power centre): L == R and non-silent
    double e = 0, diff = 0;
    for (int i = 0; i < 4800; ++i) {
        e += std::abs(buf[size_t(i) * 2]);
        diff += std::abs(buf[size_t(i) * 2] - buf[size_t(i) * 2 + 1]);
    }
    CHECK(e / 4800 > 0.01);
    CHECK(diff < 1e-3);
    // a random read equals the same range read sequentially
    std::vector<float> seq(4800 * 2), rnd(4800 * 2);
    a.read(96000, seq.data(), 4800);
    a.read(10, buf.data(), 100);
    a.read(96000, rnd.data(), 4800);
    double maxd = 0;
    for (size_t i = 0; i < seq.size(); ++i) maxd = std::max(maxd, double(std::abs(seq[i] - rnd[i])));
    CHECK(maxd < 0.02);
}

TEST_CASE("time stretch keeps duration") {
    TempoStretcher t(0.5);
    REQUIRE(t.ok());
    std::vector<float> in(48000 * 2);
    for (int i = 0; i < 48000; ++i) in[size_t(i) * 2] = in[size_t(i) * 2 + 1] = float(std::sin(i * 0.05));
    t.push(in.data(), 48000);
    t.flushInput();
    std::vector<float> out(200000 * 2);
    int got = t.pull(out.data(), 200000);
    CHECK(got == doctest::Approx(96000).epsilon(0.02));
}
TEST_CASE("scene detection finds the two cuts in scenes.mp4") {
    const std::string p = std::string(CUTLINE_TESTMEDIA) + "/scenes.mp4";
    if (!std::filesystem::exists(p)) return;
    MediaInfo mi = probeMedia(p);
    REQUIRE(mi.valid);
    std::vector<Tick> cuts;
    REQUIRE(detectScenes(p, mi, 0, mi.duration, SceneOptions{}, cuts, {}, {}));
    REQUIRE(cuts.size() == 2);
    CHECK(std::llabs(cuts[0] - 2 * kTicksPerSecond) < kTicksPerSecond / 10);
    CHECK(std::llabs(cuts[1] - 4 * kTicksPerSecond) < kTicksPerSecond / 10);
    // the rule ignores steady motion: equal scores never count as cuts
    std::vector<double> flat(50, 0.4);
    std::vector<Tick> times;
    for (int i = 0; i < 50; ++i) times.push_back(i * kTicksPerSecond / 10);
    CHECK(pickSceneCuts(flat, times, SceneOptions{}).size() <= 1);
}

#endif
