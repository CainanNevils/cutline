#include <doctest/doctest.h>

#include <algorithm>

#include "core/edits.h"
#include "core/lru.h"
#include "core/lut.h"
#include "core/project_io.h"
#include "core/srt.h"

using namespace cut;

namespace {
constexpr Tick S = kTicksPerSecond;
}

TEST_CASE("keyframe interpolation") {
    Param p;
    p.value = 7;
    CHECK(p.at(123) == 7);
    p.addKey(0, 0);
    p.addKey(100, 10);
    CHECK(p.at(-5) == 0);
    CHECK(p.at(50) == doctest::Approx(5));
    CHECK(p.at(1000) == 10);
    p.keys[0].interp = Interp::Hold;
    CHECK(p.at(99) == 0);
    p.keys[0].interp = Interp::Ease;
    CHECK(p.at(50) == doctest::Approx(5));
    CHECK(p.at(25) < 2.5);
    CHECK(p.at(75) > 7.5);
    p.addKey(50, 100);  // insert in the middle keeps order
    CHECK(p.keys[1].t == 50);
    p.set(50, 20);
    CHECK(p.keys[1].v == 20);
    CHECK(p.removeKeyAt(50));
    CHECK(p.keys.size() == 2);
    p.removeKeyAt(0);
    p.removeKeyAt(100);
    CHECK_FALSE(p.animated());
    CHECK(p.value == 10);
}

TEST_CASE("split keeps keyframes attached to content") {
    Sequence s = makeDefaultSequence();
    Clip c = makeTextClip(s.tracks[0].id, 0, 4 * S, "x");
    c.id = s.newId();
    c.opacity.params["opacity"].addKey(0, 0);
    c.opacity.params["opacity"].addKey(4 * S, 100);
    s.clips.push_back(c);
    Id r = edit::splitClip(s, c.id, 1 * S);
    const Clip* L = s.clip(c.id);
    const Clip* R = s.clip(r);
    CHECK(L->opacity.params.at("opacity").at(1 * S) == doctest::Approx(25));
    CHECK(R->opacity.params.at("opacity").at(0) == doctest::Approx(25));
    CHECK(R->opacity.params.at("opacity").at(3 * S) == doctest::Approx(100));
}

TEST_CASE("project save -> load -> save is byte-identical") {
    Project p;
    p.name = "Round trip \xC3\xA9";
    p.sequence = makeDefaultSequence({2560, 1440, {60000, 1001}, 48000});
    Bin b{p.newAssetId(), 0, "Footage"};
    p.bins.push_back(b);
    Asset a;
    a.id = p.newAssetId();
    a.binId = b.id;
    a.name = "game.mkv";
    a.path = "D:\\Videos\\game.mkv";
    a.info.valid = true;
    a.info.hasVideo = true;
    a.info.duration = 600 * S;
    a.info.video = {0, 3440, 1440, {60, 1}, {1, 1000}, "hevc", "p010le", 10, ColorRange::Limited, ColorMatrix::BT709,
                    ColorTransfer::SDR, true, false, 600 * S, 0};
    a.info.audio.push_back({1, 48000, 2, "aac", "game", "eng", 600 * S, 0});
    a.info.audio.push_back({2, 48000, 1, "opus", "mic", "", 600 * S, 0});
    p.assets.push_back(a);
    Sequence& s = p.sequence;
    auto clips = edit::clipsForAsset(s, a, 0, 10 * S, 20 * S, 0, 0);
    auto ids = edit::overwriteClips(s, clips);
    s.clip(ids[0])->motion.params["posX"].addKey(0, -440, Interp::Ease);
    s.clip(ids[0])->motion.params["posX"].addKey(5 * S, 440);
    s.clip(ids[0])->effects.push_back(makeEffect(fx::Color));
    Effect lut = makeEffect(fx::Lut);
    lut.path = "C:/luts/film.cube";
    s.clip(ids[0])->effects.push_back(lut);
    s.clip(ids[0])->speed = 0.5;
    Clip t = makeTextClip(s.tracks[1].id, 2 * S, 5 * S, "Hello\nWorld");
    t.textStyle.stroke = true;
    edit::overwriteClips(s, {t});
    edit::addCaption(s, 0, S, "first \"quoted\"");
    edit::addMarker(s, 3 * S, "beat");
    s.inPoint = S;
    s.outPoint = 9 * S;
    edit::normalize(s);
    auto rid = edit::razor(s, ids[0], 4 * S);
    edit::addTransition(s, 0, ids[0], rid[0], TransitionType::WipeLeft, S, TransitionAlign::Center);
    edit::normalize(s);

    std::string j1 = serializeProject(p, "D:\\Projects\\p.cutproj");
    Project q;
    std::string err;
    REQUIRE(deserializeProject(j1, q, err));
    std::string j2 = serializeProject(q, "D:\\Projects\\p.cutproj");
    CHECK(j1 == j2);
    CHECK(q.sequence == p.sequence);
    CHECK(q.assets[0].info == p.assets[0].info);
    CHECK(q.assets[0].relPath == "../Videos/game.mkv");

    // newer schema is refused with a clear message
    std::string newer = j1;
    newer.replace(newer.find("\"schemaVersion\": 1"), 18, "\"schemaVersion\": 99");
    CHECK_FALSE(deserializeProject(newer, q, err));
    CHECK(err.find("newer") != std::string::npos);
    CHECK_FALSE(deserializeProject("{not json", q, err));
}

TEST_CASE("every registered effect survives save and load with its keyframes") {
    Project p;
    p.sequence = makeDefaultSequence();
    const Id v = p.sequence.trackAt(TrackKind::Video, 0)->id;
    Clip c = makeTextClip(v, 0, 10 * kTicksPerSecond, "x");
    c.id = p.sequence.newId();
    int n = 0;
    for (const EffectDef& d : effectRegistry()) {
        if (d.builtin) continue;
        Effect e = makeEffect(d.type);
        for (const ParamDef& pd : d.params) {
            Param& prm = e.params[pd.id];
            prm.value = std::clamp(pd.def + pd.step, pd.min, pd.max);  // not the default
            if (pd.animatable && pd.enumLabels.empty() && !pd.isBool) {
                prm.addKey(0, pd.min, Interp::EaseIn);
                prm.addKey(kTicksPerSecond, pd.max, Interp::Hold);
            }
        }
        if (d.type == fx::Lut) e.path = "C:/luts/test.cube";
        c.effects.push_back(e);
        ++n;
    }
    p.sequence.clips.push_back(c);
    const std::string json = serializeProject(p, "C:/p/x.cutproj");
    Project back;
    std::string err;
    REQUIRE(deserializeProject(json, back, err));
    REQUIRE(back.sequence.clips.size() == 1);
    const auto& got = back.sequence.clips[0].effects;
    REQUIRE(int(got.size()) == n);
    for (size_t i = 0; i < got.size(); ++i) {
        INFO(got[i].type);
        CHECK(got[i] == c.effects[i]);
    }
}

TEST_CASE("srt parse/write round trip") {
    std::string src = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello\r\nworld\r\n\r\n"
                      "2\r\n00:01:00.25 --> 00:01:02,000\r\nSecond\r\n";
    std::vector<SrtEntry> e;
    std::string err;
    REQUIRE(parseSrt(src, e, err));
    REQUIRE(e.size() == 2);
    CHECK(e[0].start == S);
    CHECK(e[0].end == S * 5 / 2);
    CHECK(e[0].text == "Hello\nworld");
    CHECK(e[1].start == 60 * S + S / 4);
    std::string out = writeSrt(e);
    std::vector<SrtEntry> e2;
    REQUIRE(parseSrt(out, e2, err));
    CHECK(e == e2);
    CHECK(writeSrt(e2) == out);
    std::vector<SrtEntry> e3;
    REQUIRE(parseSrt(writeVtt(e), e3, err));
    CHECK(e3 == e);
    CHECK_FALSE(parseSrt("garbage", e3, err));
}

TEST_CASE("lru evicts least recently used within budget") {
    LruCache<int, int> c(10);
    std::vector<int> evicted;
    c.onEvict([&](const int& k, int&) { evicted.push_back(k); });
    c.put(1, 1, 4);
    c.put(2, 2, 4);
    CHECK(c.get(1));  // 1 is now most recent
    c.put(3, 3, 4);   // evicts 2
    CHECK(evicted == std::vector<int>{2});
    CHECK(c.contains(1));
    CHECK_FALSE(c.contains(2));
    c.put(4, 4, 20);  // larger than budget: keeps only the new one
    CHECK(c.size() == 1);
    CHECK(c.contains(4));
    c.setBudget(0);
    CHECK(c.size() == 1);
}

TEST_CASE("cube lut") {
    std::string cube = "TITLE \"id\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    Lut3D l;
    std::string err;
    REQUIRE(parseCube(cube, l, err));
    CHECK(l.title == "id");
    float in[3] = {0.25f, 0.5f, 0.75f}, out[3];
    applyLut(l, in, out);
    CHECK(out[0] == doctest::Approx(0.25));
    CHECK(out[1] == doctest::Approx(0.5));
    CHECK(out[2] == doctest::Approx(0.75));
    std::string inv = "LUT_1D_SIZE 2\n1 1 1\n0 0 0\n";
    REQUIRE(parseCube(inv, l, err));
    applyLut(l, in, out);
    CHECK(out[0] == doctest::Approx(0.75).epsilon(0.01));
    CHECK_FALSE(parseCube("LUT_3D_SIZE 2\n0 0 0\n", l, err));
}

TEST_CASE("ease in / ease out keyframe curves") {
    Param p;
    p.addKey(0, 0, Interp::EaseIn);
    p.addKey(100, 100);
    CHECK(p.at(50) == doctest::Approx(25));  // u^2
    p.keys[0].interp = Interp::EaseOut;
    CHECK(p.at(50) == doctest::Approx(75));  // 1 - (1-u)^2
    p.keys[0].interp = Interp::Ease;
    CHECK(p.at(50) == doctest::Approx(50));  // symmetric
}
