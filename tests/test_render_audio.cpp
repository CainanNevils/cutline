// Render + mixer tests on real media (need testmedia/ and a GPU).
#ifdef CUTLINE_HAVE_MEDIA
#include <doctest/doctest.h>

#include "audio/mixer.h"
#include "core/edits.h"
#include "engine/frame_renderer.h"
#include "engine/sync_provider.h"
#include "media/probe.h"

#include <QGuiApplication>

#include <cmath>
#include <filesystem>
#include <map>

using namespace cut;

namespace {
constexpr Tick S = kTicksPerSecond;
std::string media(const char* n) { return std::string(CUTLINE_TESTMEDIA) + "/" + n; }

struct Env {
    std::shared_ptr<GpuDevice> gpu;
    Compositor comp;
    Env() {
        std::string e;
        gpu = GpuDevice::create(false, e);
        if (gpu) comp.init(gpu, e);
    }
};
Env& env() {
    static Env e;
    return e;
}

RenderSnapshot snapWith(const char* file, Tick srcIn, Tick dur, bool reverse, double speed = 1.0) {
    RenderSnapshot s;
    s.seq = makeDefaultSequence({1280, 720, {60, 1}, 48000});
    AssetRef r;
    r.id = 1;
    r.path = media(file);
    r.info = probeMedia(r.path);
    s.assets[1] = r;
    Asset a;
    a.id = 1;
    a.info = r.info;
    Clip c = makeVideoClip(a, s.seq.trackAt(TrackKind::Video, 0)->id, 0, srcIn, dur);
    c.id = s.seq.newId();
    c.reverse = reverse;
    c.speed = speed;
    s.seq.clips.push_back(c);
    return s;
}

std::vector<uint8_t> renderFrame(const RenderSnapshot& s, Tick t, SyncFrameProvider& prov) {
    FrameRenderer r(env().comp);
    auto canvas = env().comp.makeCanvas(s.seq.settings.width, s.seq.settings.height, false);
    r.render(s, t, *canvas, prov, false);
    std::vector<uint8_t> d;
    int pitch = 0;
    env().comp.readback(*canvas, d, pitch);
    return d;
}
} // namespace

TEST_CASE("reversed clip is frame-exact (frame k of reverse == frame N-1-k of forward)") {
    if (!std::filesystem::exists(media("main_1440p60_h264.mkv")) || !env().gpu) return;
    const Tick dur = 2 * S;  // 120 frames
    RenderSnapshot fwd = snapWith("main_1440p60_h264.mkv", 10 * S, dur, false);
    RenderSnapshot rev = snapWith("main_1440p60_h264.mkv", 10 * S, dur, true);
    SyncFrameProvider pf(env().gpu, true), pr(env().gpu, true);
    const Tick fd = fwd.seq.frameDur();
    const int n = int(dur / fd);
    int mismatches = 0;
    for (int k = 0; k < n; k += 7) {
        auto a = renderFrame(rev, k * fd, pr);
        auto b = renderFrame(fwd, (n - 1 - k) * fd, pf);
        if (a != b) ++mismatches;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("meters read a known tone level") {
    if (!std::filesystem::exists(media("music.mp3"))) return;
    // music.mp3: 392 Hz sine at FFmpeg's default amplitude 1/8 (-18.06 dBFS), upmixed to stereo with
    // -ac 2 (-3.01 dB per channel) = -21.07 dBFS per channel
    RenderSnapshot s;
    s.seq = makeDefaultSequence();
    AssetRef r;
    r.id = 1;
    r.path = media("music.mp3");
    r.info = probeMedia(r.path);
    REQUIRE(r.info.valid);
    s.assets[1] = r;
    Asset a;
    a.id = 1;
    a.info = r.info;
    Clip c = makeAudioClip(a, 0, s.seq.trackAt(TrackKind::Audio, 0)->id, 0, 0, 10 * S);
    c.id = s.seq.newId();
    s.seq.clips.push_back(c);
    Mixer m;
    std::vector<float> buf(4800 * 2);
    for (int i = 0; i < 20; ++i) m.render(s, 48000 + i * 4800, buf.data(), 4800);
    MeterSnapshot ms = m.takeMeters();
    double db = 20 * std::log10(std::max(1e-9f, ms.master[0]));
    CHECK(db == doctest::Approx(-21.07).epsilon(0.025));  // within ~0.5 dB
    // -6 dB on the track fader reads 6 dB lower
    s.seq.tracks[size_t(s.seq.trackIndexInKind(c.trackId) + 3)].volumeDb = -6;  // A1 sits after V1..V3
    for (int i = 0; i < 10; ++i) m.render(s, 48000 + i * 4800, buf.data(), 4800);
    ms = m.takeMeters();
    double db2 = 20 * std::log10(std::max(1e-9f, ms.master[0]));
    CHECK(db2 == doctest::Approx(db - 6).epsilon(0.02));
    // mute silences
    for (auto& t : s.seq.tracks)
        if (t.kind == TrackKind::Audio) t.muted = true;
    m.render(s, 48000, buf.data(), 4800);
    ms = m.takeMeters();
    CHECK(ms.master[0] == 0.f);
}

TEST_CASE("constant-power crossfade keeps level and fades work") {
    if (!std::filesystem::exists(media("music.mp3"))) return;
    RenderSnapshot s;
    s.seq = makeDefaultSequence();
    AssetRef r;
    r.id = 1;
    r.path = media("music.mp3");
    r.info = probeMedia(r.path);
    s.assets[1] = r;
    Asset a;
    a.id = 1;
    a.info = r.info;
    Id a1 = s.seq.trackAt(TrackKind::Audio, 0)->id;
    Clip c1 = makeAudioClip(a, 0, a1, 0, 0, 4 * S);
    c1.id = s.seq.newId();
    c1.fadeIn = S;
    Clip c2 = makeAudioClip(a, 0, a1, 4 * S, 4 * S, 4 * S);
    c2.id = s.seq.newId();
    s.seq.clips.push_back(c1);
    s.seq.clips.push_back(c2);
    REQUIRE(edit::addTransition(s.seq, a1, c1.id, c2.id, TransitionType::ConstantPower, S, TransitionAlign::Center));
    Mixer m;
    std::vector<float> buf(2400 * 2);
    // during the fade-in at 0.25 s the level is ~25 %
    m.render(s, ticksToSamples(S / 4, 48000), buf.data(), 2400);
    float fadePk = m.takeMeters().master[0];
    m.render(s, ticksToSamples(2 * S, 48000), buf.data(), 2400);
    float fullPk = m.takeMeters().master[0];
    CHECK(fadePk / fullPk == doctest::Approx(0.27).epsilon(0.25));
    // in the middle of the crossfade both clips play at cos/sin(45 deg): for correlated sine the
    // sum is ~1.41x (constant power), never a dip
    m.render(s, ticksToSamples(4 * S - S / 100, 48000), buf.data(), 480);
    float midPk = m.takeMeters().master[0];
    CHECK(midPk > fullPk * 0.9f);
}

TEST_CASE("ducking lowers a track while its key track plays") {
    if (!std::filesystem::exists(media("music.mp3"))) return;
    RenderSnapshot s;
    s.seq = makeDefaultSequence();
    AssetRef r;
    r.id = 1;
    r.path = media("music.mp3");
    r.info = probeMedia(r.path);
    s.assets[1] = r;
    Asset a;
    a.id = 1;
    a.info = r.info;
    const Id a1 = s.seq.trackAt(TrackKind::Audio, 0)->id, a2 = s.seq.trackAt(TrackKind::Audio, 1)->id;
    Clip music = makeAudioClip(a, 0, a1, 0, 0, 10 * S);
    music.id = s.seq.newId();
    Clip voice = makeAudioClip(a, 0, a2, 4 * S, 0, 2 * S);  // "voice" on A2 from 4 s to 6 s
    voice.id = s.seq.newId();
    s.seq.clips.push_back(music);
    s.seq.clips.push_back(voice);
    for (auto& t : s.seq.tracks)
        if (t.id == a1) {
            t.duckUnder = a2;
            t.duckDb = -12;
        }
    Mixer m;
    std::vector<float> buf(4800 * 2);
    std::map<int, double> a1Db;  // block (100 ms) -> A1 peak dB
    for (int b = 10; b < 95; ++b) {
        m.render(s, int64_t(b) * 4800, buf.data(), 4800);
        for (const auto& [id, pk] : m.takeMeters().tracks)
            if (id == a1) a1Db[b] = 20 * std::log10(std::max(1e-9f, pk[0]));
    }
    CHECK(a1Db[30] == doctest::Approx(-21.07).epsilon(0.03));   // before the voice: full level
    CHECK(a1Db[55] == doctest::Approx(-33.07).epsilon(0.03));   // under the voice: -12 dB
    CHECK(a1Db[90] == doctest::Approx(-21.07).epsilon(0.05));   // recovered after it
    CHECK(a1Db[62] < a1Db[90] - 3);                               // still released smoothly, not a jump
}

TEST_CASE("a nested sequence's audio mixes like its contents") {
    if (!std::filesystem::exists(media("music.mp3"))) return;
    AssetRef r;
    r.id = 1;
    r.path = media("music.mp3");
    r.info = probeMedia(r.path);
    Asset a;
    a.id = 1;
    a.info = r.info;
    auto inner = std::make_shared<RenderSnapshot>();
    inner->seq = makeDefaultSequence();
    inner->seq.uid = 9;
    inner->assets[1] = r;
    Clip m = makeAudioClip(a, 0, inner->seq.trackAt(TrackKind::Audio, 0)->id, 0, 0, 10 * S);
    m.id = inner->seq.newId();
    inner->seq.clips.push_back(m);

    RenderSnapshot outer;
    outer.seq = makeDefaultSequence();
    outer.assets[1] = r;
    outer.nested[9] = inner;
    Clip n;
    n.kind = ClipKind::Nested;
    n.nestedSeq = 9;
    n.trackId = outer.seq.trackAt(TrackKind::Audio, 0)->id;
    n.start = 2 * S;      // placed at 2 s, showing the nest from 1 s on
    n.sourceIn = S;
    n.duration = 5 * S;
    initBuiltinEffects(n, TrackKind::Audio);
    n.id = outer.seq.newId();
    outer.seq.clips.push_back(n);

    Mixer direct, nested;
    std::vector<float> a1(4800 * 2), b1(4800 * 2);
    float worst = 0;
    for (int blk = 0; blk < 20; ++blk) {
        nested.render(outer, 3 * 48000 + blk * 4800, b1.data(), 4800);         // outer 3 s .. 5 s
        direct.render(*inner, 2 * 48000 + blk * 4800, a1.data(), 4800);       // = nest 2 s .. 4 s
        for (size_t i = 0; i < a1.size(); ++i) worst = std::max(worst, std::abs(a1[i] - b1[i]));
    }
    CHECK(worst < 1e-5f);
    std::vector<float> silent(4800 * 2);
    nested.render(outer, 0, silent.data(), 4800);  // before the nest clip: silence
    CHECK(*std::max_element(silent.begin(), silent.end()) == 0.f);
}

TEST_CASE("DeNoise does not shift a clip in time (the mixer reads ahead by its latency)") {
    if (!std::filesystem::exists(media("pauses.wav"))) return;
    AssetRef r;
    r.id = 1;
    r.path = media("pauses.wav");
    r.info = probeMedia(r.path);
    Asset a;
    a.id = 1;
    a.info = r.info;
    auto onset = [&](bool denoise) {
        RenderSnapshot s;
        s.seq = makeDefaultSequence();
        s.assets[1] = r;
        Clip c = makeAudioClip(a, 0, s.seq.trackAt(TrackKind::Audio, 0)->id, 0, 0, 4 * S);
        c.id = s.seq.newId();
        if (denoise) c.effects.push_back(makeEffect(fx::Denoise));
        s.seq.clips.push_back(c);
        Mixer m;
        std::vector<float> buf(480 * 2);
        // tone resumes at 2.5 s after 1.5 s of silence: first sample above -20 dBFS after 2.2 s
        for (int64_t pos = 0; pos < 3 * 48000; pos += 480) {
            m.render(s, pos, buf.data(), 480);
            if (pos < 2 * 48000 + 9600) continue;
            for (int i = 0; i < 480; ++i)
                if (std::abs(buf[size_t(i) * 2]) > 0.1f) return pos + i;
        }
        return int64_t(-1);
    };
    const int64_t plain = onset(false), cleaned = onset(true);
    REQUIRE(plain > 0);
    CHECK(std::llabs(cleaned - plain) < 48);  // within 1 ms
}
#endif
