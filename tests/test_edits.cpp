#include <doctest/doctest.h>

#include <set>

#include "core/edits.h"
#include "core/evaluate.h"
#include "core/undo.h"

#include <cmath>

using namespace cut;

namespace {
constexpr Tick S = kTicksPerSecond;
constexpr Tick F = S / 60;  // 60 fps frame

struct Fixture {
    Project p;
    Sequence& s = p.sequence;
    Id v1, v2, a1, a2;
    Fixture() {
        s = makeDefaultSequence({1920, 1080, {60, 1}, 48000});
        v1 = s.trackAt(TrackKind::Video, 0)->id;
        v2 = s.trackAt(TrackKind::Video, 1)->id;
        a1 = s.trackAt(TrackKind::Audio, 0)->id;
        a2 = s.trackAt(TrackKind::Audio, 1)->id;
        Asset a;
        a.id = p.newAssetId();
        a.name = "clip";
        a.path = "C:/x.mp4";
        a.info.valid = true;
        a.info.hasVideo = true;
        a.info.duration = 10 * S;
        a.info.video.width = 1920;
        a.info.video.height = 1080;
        a.info.audio.push_back({1, 48000, 2});
        p.assets.push_back(a);
    }
    const Asset& asset() const { return p.assets[0]; }
    // video+audio linked clip at [start, start+dur) from source srcIn
    Id place(Tick start, Tick dur, Tick srcIn = 0) {
        auto clips = edit::clipsForAsset(s, asset(), start, srcIn, srcIn + dur, 0, 0);
        auto ids = edit::overwriteClips(s, clips);
        edit::normalize(s);
        return ids.front();
    }
    std::vector<const Clip*> on(Id track) const { return s.clipsOnTrack(track); }
};
} // namespace

TEST_CASE("place creates linked video + audio") {
    Fixture f;
    Id v = f.place(0, 5 * S);
    auto linked = f.s.linkedClips(v);
    CHECK(linked.size() == 2);
    CHECK(f.on(f.a1).size() == 1);
    CHECK(f.on(f.a1)[0]->duration == 5 * S);
}

TEST_CASE("razor splits clip and linked audio") {
    Fixture f;
    Id v = f.place(0, 5 * S, 1 * S);
    auto right = edit::razor(f.s, v, 2 * S);
    edit::normalize(f.s);
    CHECK(right.size() == 2);
    auto vc = f.on(f.v1);
    REQUIRE(vc.size() == 2);
    CHECK(vc[0]->start == 0);
    CHECK(vc[0]->duration == 2 * S);
    CHECK(vc[0]->sourceIn == 1 * S);
    CHECK(vc[1]->start == 2 * S);
    CHECK(vc[1]->duration == 3 * S);
    CHECK(vc[1]->sourceIn == 3 * S);
    // right parts are linked to each other, not to the left parts
    CHECK(f.s.linkedClips(vc[1]->id).size() == 2);
    CHECK(vc[0]->linkId != vc[1]->linkId);
    CHECK(f.on(f.a1).size() == 2);
}

TEST_CASE("razor on reverse clip keeps the mirrored source ranges") {
    Fixture f;
    Id v = f.place(0, 4 * S, 2 * S);  // source [2s, 6s)
    f.s.clip(v)->reverse = true;
    edit::razor(f.s, v, 1 * S, true);
    auto vc = f.on(f.v1);
    REQUIRE(vc.size() == 2);
    // left part shows source [5s,6s) backwards, right part [2s,5s)
    CHECK(vc[0]->sourceIn == 5 * S);
    CHECK(vc[0]->sourceOut() == 6 * S);
    CHECK(vc[1]->sourceIn == 2 * S);
    CHECK(vc[1]->sourceOut() == 5 * S);
    // first timeline frame of a reversed clip shows the last source frame
    CHECK(vc[0]->sourceAt(0, F) == 6 * S - F);
}

TEST_CASE("ripple delete closes the gap on all tracks") {
    Fixture f;
    Id a = f.place(0, 2 * S);
    Id b = f.place(2 * S, 2 * S, 2 * S);
    Id c = f.place(4 * S, 2 * S, 4 * S);
    (void)a;
    auto ids = f.s.linkedClips(b);
    edit::rippleDelete(f.s, ids);
    edit::normalize(f.s);
    CHECK(f.s.clip(b) == nullptr);
    CHECK(f.s.clip(c)->start == 2 * S);
    auto ac = f.on(f.a1);
    REQUIRE(ac.size() == 2);
    CHECK(ac[1]->start == 2 * S);
}

TEST_CASE("ripple delete is limited by other tracks") {
    Fixture f;
    f.place(0, 2 * S);
    Id b = f.place(2 * S, 2 * S);
    Id c = f.place(4 * S, 2 * S);
    // V2 has a clip spanning 1s..5s and another at 6s: shifting V2 by 2s would collide at 3s..5s -> 1s max
    Clip x = makeVideoClip(f.asset(), f.v2, 1 * S, 0, 4 * S);
    Clip y = makeVideoClip(f.asset(), f.v2, 6 * S, 0, 1 * S);
    edit::overwriteClips(f.s, {x, y});
    edit::rippleDelete(f.s, f.s.linkedClips(b));
    edit::normalize(f.s);
    CHECK(f.s.clip(c)->start == 3 * S);
}

TEST_CASE("insert pushes later material right") {
    Fixture f;
    Id v = f.place(0, 5 * S);
    auto clips = edit::clipsForAsset(f.s, f.asset(), 3 * S, 7 * S, 8 * S, 0, 0);
    edit::insertClips(f.s, clips, 3 * S);
    edit::normalize(f.s);
    auto vc = f.on(f.v1);
    REQUIRE(vc.size() == 3);
    CHECK(vc[0]->id == v);
    CHECK(vc[0]->end() == 3 * S);
    CHECK(vc[1]->start == 3 * S);
    CHECK(vc[1]->sourceIn == 7 * S);
    CHECK(vc[2]->start == 4 * S);
    CHECK(vc[2]->sourceIn == 3 * S);
    CHECK(vc[2]->end() == 6 * S);
    CHECK(f.on(f.a1).size() == 3);
}

TEST_CASE("overwrite replaces media in range") {
    Fixture f;
    f.place(0, 5 * S);
    auto clips = edit::clipsForAsset(f.s, f.asset(), 3 * S, 7 * S, 8 * S, 0, 0);
    edit::overwriteClips(f.s, clips);
    edit::normalize(f.s);
    auto vc = f.on(f.v1);
    REQUIRE(vc.size() == 3);
    CHECK(vc[0]->end() == 3 * S);
    CHECK(vc[1]->start == 3 * S);
    CHECK(vc[1]->end() == 4 * S);
    CHECK(vc[2]->start == 4 * S);
    CHECK(vc[2]->sourceIn == 4 * S);
    CHECK(vc[2]->end() == 5 * S);
}

TEST_CASE("trim respects media length and neighbours") {
    Fixture f;
    Id a = f.place(0, 4 * S, 2 * S);
    Id b = f.place(6 * S, 2 * S, 0);
    // tail: media ends at 10s, source [2,6) -> can extend 4s, but neighbour at 6s
    edit::trimClip(f.s, f.p.assets, a, edit::Edge::Tail, 20 * S, false);
    CHECK(f.s.clip(a)->end() == 6 * S);
    // head: source starts at 2s -> can extend 2s back but timeline floor is 0
    edit::trimClip(f.s, f.p.assets, a, edit::Edge::Head, -5 * S, false);
    CHECK(f.s.clip(a)->start == 0);
    CHECK(f.s.clip(a)->sourceIn == 2 * S);
    edit::trimClip(f.s, f.p.assets, a, edit::Edge::Head, 1 * S, false);
    CHECK(f.s.clip(a)->start == 1 * S);
    CHECK(f.s.clip(a)->sourceIn == 3 * S);
    // linked audio followed
    auto ac = f.on(f.a1);
    CHECK(ac[0]->start == 1 * S);
    // min 1 frame
    edit::trimClip(f.s, f.p.assets, b, edit::Edge::Tail, 0, false);
    CHECK(f.s.clip(b)->duration == F);
}

TEST_CASE("ripple trim shifts following clips") {
    Fixture f;
    Id a = f.place(0, 4 * S);
    Id b = f.place(4 * S, 2 * S, 4 * S);
    edit::trimClip(f.s, f.p.assets, a, edit::Edge::Tail, 3 * S, true);
    CHECK(f.s.clip(a)->end() == 3 * S);
    CHECK(f.s.clip(b)->start == 3 * S);
    edit::trimClip(f.s, f.p.assets, a, edit::Edge::Tail, 5 * S, true);
    CHECK(f.s.clip(a)->end() == 5 * S);
    CHECK(f.s.clip(b)->start == 5 * S);
    // ripple head: clip start stays, content moves, followers shift
    edit::trimClip(f.s, f.p.assets, a, edit::Edge::Head, 1 * S, true);
    CHECK(f.s.clip(a)->start == 0);
    CHECK(f.s.clip(a)->sourceIn == 1 * S);
    CHECK(f.s.clip(a)->duration == 4 * S);
    CHECK(f.s.clip(b)->start == 4 * S);
}

TEST_CASE("move overwrites destination") {
    Fixture f;
    Id a = f.place(0, 2 * S);
    Id b = f.place(4 * S, 4 * S);
    auto r = edit::moveClips(f.s, f.s.linkedClips(a), 5 * S, 0, 0);
    edit::normalize(f.s);
    CHECK(r.ok);
    CHECK(f.s.clip(a)->start == 5 * S);
    // b was split around [5,7)
    auto vc = f.on(f.v1);
    REQUIRE(vc.size() == 3);
    CHECK(vc[0]->id == b);
    CHECK(vc[0]->end() == 5 * S);
    CHECK(vc[2]->start == 7 * S);
    CHECK(vc[2]->end() == 8 * S);
    // move to V2 (only video moves track)
    r = edit::moveClips(f.s, {a}, 0, 1, 0);
    CHECK(r.ok);
    CHECK(f.s.clip(a)->trackId == f.v2);
    // invalid target track
    r = edit::moveClips(f.s, {a}, 0, 5, 0);
    CHECK_FALSE(r.ok);
}

TEST_CASE("speed change scales duration and ripples") {
    Fixture f;
    Id a = f.place(0, 2 * S);
    Id b = f.place(2 * S, 1 * S);
    edit::setSpeed(f.s, f.p.assets, a, 0.5, false, true, false);
    CHECK(f.s.clip(a)->duration == 4 * S);
    CHECK(f.s.clip(a)->sourceSpan() == 2 * S);
    CHECK(f.s.clip(b)->start == 4 * S);
    // without ripple, growth is limited to free space
    edit::setSpeed(f.s, f.p.assets, a, 0.25, false, false, false);
    CHECK(f.s.clip(a)->duration == 4 * S);
}

TEST_CASE("frame hold") {
    Fixture f;
    Id a = f.place(0, 4 * S, 1 * S);
    Id h = edit::addFrameHold(f.s, a, 1 * S);
    REQUIRE(h);
    const Clip* hc = f.s.clip(h);
    CHECK(hc->isHold());
    CHECK(hc->sourceIn == 2 * S);
    CHECK(hc->sourceAt(3 * S) == 2 * S);
    CHECK(f.s.clip(a)->end() == 1 * S);
}

TEST_CASE("transitions are dropped when clips separate and clamp to handles") {
    Fixture f;
    Id a = f.place(0, 2 * S);
    Id b = f.place(2 * S, 2 * S, 2 * S);
    Id t = edit::addTransition(f.s, f.v1, a, b, TransitionType::CrossDissolve, S, TransitionAlign::Center);
    REQUIRE(t);
    TickRange r = transitionRange(f.s, *f.s.transition(t));
    CHECK(r.a == 1.5 * S);
    CHECK(r.b == 2.5 * S);
    CHECK_FALSE(edit::transitionMissingHandles(f.s, f.p.assets, *f.s.transition(t)));
    // clip a starts at source 0: incoming b needs 0.5s before 2s -> fine; a needs 0.5s after 2s -> fine
    auto layers = evaluateVideo(f.s, 2 * S);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].transition);
    CHECK(layers[0].progress == doctest::Approx(0.5));
    CHECK(layers[0].a.source == 2 * S);
    CHECK(layers[0].b.source == 2 * S);
    edit::moveClips(f.s, {b}, 1 * S, 0, 0);
    edit::normalize(f.s);
    CHECK(f.s.transitions.empty());
}

TEST_CASE("undo/redo round trips every operation") {
    Document d;
    Fixture f;
    d.reset(f.p);
    const Project initial = d.project;
    std::vector<Project> states{initial};
    Id v = 0;
    d.edit("place", [&](Project& p) {
        auto clips = edit::clipsForAsset(p.sequence, p.assets[0], 0, 0, 6 * S, 0, 0);
        v = edit::overwriteClips(p.sequence, clips).front();
    });
    states.push_back(d.project);
    d.edit("razor", [&](Project& p) { edit::razor(p.sequence, v, 2 * S); });
    states.push_back(d.project);
    d.edit("speed", [&](Project& p) { edit::setSpeed(p.sequence, p.assets, v, 2.0, true, true, true); });
    states.push_back(d.project);
    d.edit("marker", [&](Project& p) { edit::addMarker(p.sequence, 3 * S, "m"); });
    states.push_back(d.project);
    d.edit("caption", [&](Project& p) { edit::addCaption(p.sequence, 0, S, "hello"); });
    states.push_back(d.project);
    d.edit("track", [&](Project& p) { edit::addTrack(p.sequence, TrackKind::Video); });
    states.push_back(d.project);
    d.edit("key", [&](Project& p) { p.sequence.clip(v)->opacity.params["opacity"].addKey(F * 10, 50); });
    states.push_back(d.project);
    CHECK(d.undo.size() == states.size() - 1);
    // a no-op edit adds nothing
    CHECK_FALSE(d.edit("noop", [](Project&) {}));
    for (size_t i = states.size() - 1; i > 0; --i) {
        REQUIRE(d.doUndo());
        CHECK(d.project.sequence == states[i - 1].sequence);
    }
    CHECK_FALSE(d.doUndo());
    for (size_t i = 1; i < states.size(); ++i) {
        REQUIRE(d.doRedo());
        CHECK(d.project.sequence == states[i].sequence);
    }
    // a drag is one undo step
    d.begin("drag");
    for (int i = 0; i < 10; ++i) edit::moveClips(d.project.sequence, {v}, F, 0, 0);
    d.commit();
    CHECK(d.undo.undoLabel() == "drag");
    d.doUndo();
    CHECK(d.project.sequence == states.back().sequence);
}

TEST_CASE("ensure tracks and clips for multi-stream assets") {
    Fixture f;
    f.p.assets[0].info.audio.push_back({2, 44100, 1, "aac", "mic"});
    auto clips = edit::clipsForAsset(f.s, f.asset(), 0, 0, S, 0, 0);
    CHECK(clips.size() == 3);
    CHECK(clips[1].trackId == f.a1);
    CHECK(clips[2].trackId == f.a2);
    CHECK(clips[2].stream == 1);
    CHECK(clips[0].linkId == clips[2].linkId);
    // asset with 4 audio streams creates a 4th audio track
    f.p.assets[0].info.audio.push_back({3, 48000, 2});
    f.p.assets[0].info.audio.push_back({4, 48000, 2});
    clips = edit::clipsForAsset(f.s, f.asset(), 0, 0, S, 0, 0);
    CHECK(f.s.tracksOf(TrackKind::Audio).size() == 4);
}

TEST_CASE("animation presets write editable keyframes") {
    Fixture f;
    const Id v = f.place(0, 4 * S);
    Clip& c = *f.s.clip(v);
    REQUIRE(edit::applyAnimation(c, edit::Animation::FadeIn, F, 1920, 1080));
    const Param& op = c.opacity.params.at("opacity");
    CHECK(op.at(0) == doctest::Approx(0));
    CHECK(op.at(S) == doctest::Approx(100));  // full after ~0.4 s
    REQUIRE(edit::applyAnimation(c, edit::Animation::PopIn, F, 1920, 1080));
    const Param& sc = c.motion.params.at("scale");
    CHECK(sc.at(0) == doctest::Approx(0));
    CHECK(sc.at(S) == doctest::Approx(100));
    REQUIRE(edit::applyAnimation(c, edit::Animation::SlideLeftIn, F, 1920, 1080));
    CHECK(c.motion.params.at("posX").at(0) == doctest::Approx(-1920));
    // audio clips have no motion: nothing to animate
    Clip* audio = nullptr;
    for (auto& x : f.s.clips)
        if (x.trackId == f.a1) audio = &x;
    REQUIRE(audio);
    CHECK_FALSE(edit::applyAnimation(*audio, edit::Animation::FadeIn, F, 1920, 1080));
    // Typewriter: titles only, progress 0 -> 100 % at about 30 characters a second
    CHECK_FALSE(edit::applyAnimation(c, edit::Animation::Typewriter, F, 1920, 1080));
    Clip t = makeTextClip(f.v1, 0, 4 * S, "Thirty characters of a title!!");
    REQUIRE(edit::applyAnimation(t, edit::Animation::Typewriter, F, 1920, 1080));
    REQUIRE(t.effects.size() == 1);
    const Param& pr = t.effects[0].params.at("progress");
    CHECK(pr.at(0) == doctest::Approx(0));
    CHECK(pr.at(S / 2) == doctest::Approx(50).epsilon(0.05));
    CHECK(pr.at(S + F) == doctest::Approx(100));
    REQUIRE(edit::applyAnimation(t, edit::Animation::Typewriter, F, 1920, 1080));  // again: same effect, new keys
    CHECK(t.effects.size() == 1);
}

TEST_CASE("silence detection finds pauses and keeps padding") {
    // 10 ms windows: speech 0-1 s, silence 1-2.5 s, speech 2.5-3 s, short pause 3-3.2 s, speech to 4 s
    const Tick win = S / 100;
    std::vector<float> lv(400, 0.5f);
    for (int i = 100; i < 250; ++i) lv[size_t(i)] = 0.001f;  // -60 dB
    for (int i = 300; i < 320; ++i) lv[size_t(i)] = 0.001f;
    auto spans = edit::findSilences(lv, 0, win, -40, S / 2, S / 10, F);
    REQUIRE(spans.size() == 1);  // the 0.2 s pause is too short
    CHECK(spans[0].a == S + S / 10);
    CHECK(spans[0].b == 2 * S + S / 2 - S / 10);
    // removing it ripples everything after it left on all tracks
    Fixture f;
    const Id v = f.place(0, 4 * S);
    (void)v;
    const Tick before = f.s.duration();
    edit::removeSpans(f.s, spans);
    CHECK(f.s.duration() == before - (spans[0].b - spans[0].a));
    CHECK(f.on(f.v1).size() == 2);
    CHECK(f.on(f.a1).size() == 2);
    CHECK(f.on(f.v1)[1]->start == f.on(f.v1)[0]->end());  // no gap left
}

TEST_CASE("speed ramp builds an eased staircase around the playhead") {
    Fixture f;
    const Id v = f.place(0, 10 * S);
    const Tick before = f.s.duration();
    REQUIRE(edit::speedRamp(f.s, f.p.assets, v, 5 * S, 0.25, S, S / 2, 5));
    auto vs = f.on(f.v1);
    REQUIRE(vs.size() == 1 + 5 + 1 + 5 + 1);  // before, ramp in, slow, ramp out, after
    CHECK(vs[0]->speed == 1.0);
    CHECK(vs[6]->speed == doctest::Approx(0.25));
    CHECK(vs.back()->speed == 1.0);
    for (size_t i = 1; i < vs.size(); ++i) CHECK(vs[i]->start == vs[i - 1]->end());  // no gaps
    for (size_t i = 1; i <= 5; ++i) CHECK(vs[i]->speed < vs[i - 1]->speed);          // slowing down
    // the media still plays continuously: each piece starts where the previous one ended
    for (size_t i = 1; i < vs.size(); ++i) CHECK(std::llabs(vs[i]->sourceIn - vs[i - 1]->sourceOut()) <= F);
    CHECK(f.s.duration() > before + 2 * S);  // 1 s at 25 % alone adds 3 s
    CHECK(f.on(f.a1).size() == vs.size());   // linked audio follows
}

TEST_CASE("changing the frame rate keeps cuts shared, nothing overlapping, sources inside media") {
    Fixture f;
    // three adjacent clips with cuts on odd 60 fps frames, the last one ending at the media end
    const Id a = f.place(0, 31 * F);
    const Id b = f.place(31 * F, 47 * F, 2 * S);
    const Id c = f.place(78 * F, 10 * S - 7 * F - 78 * F, 78 * F + 7 * F);
    (void)a, (void)b, (void)c;
    edit::addMarker(f.s, 13 * F);
    edit::changeFrameRate(f.s, f.p.assets, {30, 1});
    const Tick F30 = S / 30;
    for (Id track : {f.v1, f.a1}) {
        auto cs = f.on(track);
        REQUIRE(cs.size() == 3);
        for (size_t i = 0; i < cs.size(); ++i) {
            CHECK(cs[i]->start % F30 == 0);
            CHECK(cs[i]->duration % F30 == 0);
            CHECK(cs[i]->sourceOut() <= 10 * S);
            if (i) CHECK(cs[i]->start == cs[i - 1]->end());  // still adjacent, no overlap
        }
    }
    // linked partners stay together
    auto vs = f.on(f.v1), as = f.on(f.a1);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(vs[i]->start == as[i]->start);
        CHECK(vs[i]->sourceIn == as[i]->sourceIn);
    }
    CHECK(f.s.markers.front().t % F30 == 0);
    CHECK(f.s.settings.fps == Rational{30, 1});
}

TEST_CASE("replace with clip keeps position, effects and keyframes; shortens to the new media") {
    Fixture f;
    Asset b = f.asset();
    b.id = f.p.newAssetId();
    b.name = "retake";
    b.info.duration = 5 * S;
    f.p.assets.push_back(b);
    const Id v = f.place(2 * S, 6 * S);
    f.s.clip(v)->motion.params["scale"].addKey(S, 150);
    const int n = edit::replaceMedia(f.s, b, {v}, S);
    edit::normalize(f.s);
    CHECK(n == 2);  // the video and its linked audio
    const Clip* c = f.on(f.v1).front();
    const Clip* a = f.on(f.a1).front();
    CHECK(c->assetId == b.id);
    CHECK(a->assetId == b.id);
    CHECK(c->start == 2 * S);
    CHECK(c->sourceIn == S);
    CHECK(c->duration == 4 * S);  // only 4 s of the retake after its in point
    CHECK(a->duration == 4 * S);
    CHECK(c->motion.params.at("scale").keys.size() == 1);
    // an in point past the end of the new media changes nothing
    CHECK(edit::replaceMedia(f.s, b, {c->id}, 6 * S) == 0);
}

TEST_CASE("punch-in zooms toward a point that stays put, then eases back") {
    Fixture f;
    const Id v = f.place(0, 6 * S);
    Clip& c = *f.s.clip(v);
    c.motion.params["posX"].value = 100;  // already offset
    const double px = 1500, py = 300, k = 1.5;
    REQUIRE(edit::punchIn(c, S, px, py, k, S / 2, 2 * S, F, 1920, 1080));
    const Tick peak = S + S / 2;
    CHECK(c.motion.p("scale", peak) == doctest::Approx(150));
    // the image point under (px, py) at the start is still there at the peak
    const double cx0 = 960 + 100, cx1 = 960 + c.motion.p("posX", peak);
    const double cy0 = 540, cy1 = 540 + c.motion.p("posY", peak);
    CHECK(cx1 + k * (px - cx0) == doctest::Approx(px));
    CHECK(cy1 + k * (py - cy0) == doctest::Approx(py));
    CHECK(c.motion.p("scale", 3 * S + S / 2) == doctest::Approx(150));  // holding
    CHECK(c.motion.p("scale", 4 * S) == doctest::Approx(100));          // back
    CHECK(c.motion.p("posX", 4 * S) == doctest::Approx(100));
    // too close to the end: squeezed into what is left
    Clip& d = *f.s.clip(v);
    CHECK(edit::punchIn(d, 5 * S, px, py, k, S / 2, 2 * S, F, 1920, 1080));
    CHECK(d.motion.p("scale", d.duration - F) == doctest::Approx(100));
}

TEST_CASE("fit music to the end: cut with a fade, or looped with crossfades") {
    Fixture f;
    auto place = [&](Fixture& fx, Tick start, Tick dur) {
        auto clips = edit::clipsForAsset(fx.s, fx.asset(), start, 0, dur, 0, 0, false, true);
        return edit::overwriteClips(fx.s, clips).front();
    };
    // longer than needed: cut at the end, 3 s fade out
    const Id m = place(f, 0, 10 * S);
    CHECK(edit::fitAudioToEnd(f.s, f.p.assets, m, 6 * S, 3 * S) == 1);
    CHECK(f.s.clip(m)->duration == 6 * S);
    CHECK(f.s.clip(m)->fadeOut == 3 * S);
    // shorter: 10 s of music looped to 25 s; 1 s crossfades need 1 s of tail, so repeats are 9 s
    Fixture g;
    const Id n = place(g, 0, 10 * S);
    CHECK(edit::fitAudioToEnd(g.s, g.p.assets, n, 25 * S, 3 * S) == 3);
    edit::normalize(g.s);
    auto as = g.on(g.a1);
    REQUIRE(as.size() == 3);
    CHECK(as[0]->duration == 9 * S);
    CHECK(as[1]->start == 9 * S);
    CHECK(as[1]->sourceIn == 0);
    CHECK(as[1]->duration == 9 * S);
    CHECK(as[2]->end() == 25 * S);
    CHECK(as[2]->fadeOut == 3 * S);
    REQUIRE(g.s.transitions.size() == 2);
    for (const auto& t : g.s.transitions) {
        CHECK(t.type == TransitionType::ConstantPower);
        CHECK(t.duration == S);
        CHECK_FALSE(edit::transitionMissingHandles(g.s, g.p.assets, t));
    }
    // something later on the track limits it
    Fixture h;
    const Id o = place(h, 0, 4 * S);
    place(h, 12 * S, 2 * S);
    CHECK(edit::fitAudioToEnd(h.s, h.p.assets, o, 30 * S, 2 * S) >= 2);
    edit::normalize(h.s);
    Tick last = 0;
    for (const Clip* c : h.on(h.a1))
        if (c->start < 12 * S) last = std::max(last, c->end());
    CHECK(last == 12 * S);
}

TEST_CASE("instant replay repeats the moment before the playhead in slow motion") {
    Fixture f;
    f.place(0, 10 * S);
    Clip& v = *f.s.clip(f.on(f.v1).front()->id);
    v.opacity.params["opacity"].addKey(5 * S, 100);  // a key inside the window follows the new timing
    const TickRange r = edit::instantReplay(f.s, 6 * S, 2 * S, 0.5);
    edit::normalize(f.s);
    CHECK(r.a == 6 * S);
    CHECK(r.b == 10 * S);
    auto vs = f.on(f.v1);
    auto as = f.on(f.a1);
    REQUIRE(vs.size() == 3);
    REQUIRE(as.size() == 3);
    // before | replay of 4..6 s at 50 % | the rest, rippled by 4 s
    CHECK(vs[0]->start == 0);
    CHECK(vs[0]->end() == 6 * S);
    CHECK(vs[1]->start == 6 * S);
    CHECK(vs[1]->duration == 4 * S);
    CHECK(vs[1]->speed == doctest::Approx(0.5));
    CHECK(vs[1]->sourceIn == 4 * S);
    CHECK(vs[1]->sourceOut() == 6 * S);
    CHECK(vs[2]->start == 10 * S);
    CHECK(vs[2]->sourceIn == 6 * S);
    CHECK(as[1]->start == 6 * S);
    CHECK(as[1]->speed == doctest::Approx(0.5));
    std::set<Id> ids;
    for (const auto& c : f.s.clips) ids.insert(c.id);
    CHECK(ids.size() == f.s.clips.size());  // copies are new clips
    CHECK(vs[1]->linkId != 0);
    CHECK(vs[1]->linkId == as[1]->linkId);
    CHECK(vs[1]->linkId != vs[0]->linkId);
    const auto& keys = vs[1]->opacity.params.at("opacity").keys;
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].t == 2 * S);  // was 5 s = 1 s into the window -> 2 s into the replay
    // nothing before the playhead: nothing to replay
    Fixture g;
    g.place(5 * S, 2 * S);
    CHECK(edit::instantReplay(g.s, 2 * S, 2 * S, 0.5).len() == 0);
}

TEST_CASE("highlights: loud moments stand out from their surroundings") {
    // 10 ms windows, 3 minutes of game noise; three 1 s bursts, and a loud second half where only
    // its burst (not the whole section) counts
    const Tick win = S / 100;
    std::vector<float> lv(18000);
    for (size_t i = 0; i < lv.size(); ++i) lv[i] = 0.05f + 0.02f * float(std::sin(double(i) * 0.37));
    for (size_t i = 12000; i < lv.size(); ++i) lv[i] *= 3.f;
    for (int sec : {20, 55, 140})
        for (int k = 0; k < 100; ++k) lv[size_t(sec * 100 + k)] = sec > 120 ? 0.9f : 0.6f;
    auto hl = edit::findHighlights(lv, 0, win, 10, 8 * S, F);
    // the bursts, plus at most the jump into the loud section at 120 s
    REQUIRE(hl.size() >= 3);
    REQUIRE(hl.size() <= 4);
    std::vector<Tick> bursts;
    for (Tick t : hl)
        if (std::llabs(t - 120 * S) > S) bursts.push_back(t);
    REQUIRE(bursts.size() == 3);
    CHECK(std::llabs(bursts[0] - 20 * S) <= S / 5);
    CHECK(std::llabs(bursts[1] - 55 * S) <= S / 5);
    CHECK(std::llabs(bursts[2] - 140 * S) <= S / 5);
    // maxCount keeps the strongest
    auto one = edit::findHighlights(lv, 0, win, 1, 8 * S, F);
    REQUIRE(one.size() == 1);
    // flat audio has none
    CHECK(edit::findHighlights(std::vector<float>(6000, 0.1f), 0, win, 10, 8 * S, F).empty());
}

TEST_CASE("beat detection finds regular hits") {
    // 10 ms windows, 4 s: quiet bed with a hit every 0.5 s (decaying over 100 ms)
    const Tick win = S / 100;
    std::vector<float> lv(400, 0.02f);
    for (int beat = 0; beat < 8; ++beat)
        for (int k = 0; k < 10; ++k) lv[size_t(beat * 50 + 5 + k)] = 0.8f * float(std::exp(-k / 3.0)) + 0.02f;
    auto beats = edit::findBeats(lv, 0, win, S / 4, F);
    REQUIRE(beats.size() == 8);
    for (int b = 0; b < 8; ++b) CHECK(std::llabs(beats[size_t(b)] - (b * S / 2 + 5 * win)) <= 2 * F);
}
