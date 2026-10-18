// Randomized editing: thousands of random edit operations (the same calls the UI makes), with
// model invariants checked after every one, then undo-all / redo-all must reproduce each state.
#include <doctest/doctest.h>

#include "core/edits.h"
#include "core/project_io.h"
#include "core/undo.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <sstream>

using namespace cut;

namespace {

constexpr Tick S = kTicksPerSecond;
constexpr Tick F = S / 60;

struct Fuzzer {
    std::mt19937 rng;
    Document d;
    std::string lastOp, lastDetail;

    explicit Fuzzer(unsigned seed) : rng(seed) {
        Project p;
        p.sequence = makeDefaultSequence({1920, 1080, {60, 1}, 48000});
        for (int i = 0; i < 3; ++i) {
            Asset a;
            a.id = p.newAssetId();
            a.name = "clip" + std::to_string(i);
            a.path = "C:/fuzz" + std::to_string(i) + ".mp4";
            a.info.valid = true;
            a.info.hasVideo = true;
            a.info.duration = (8 + 6 * i) * S;
            a.info.video.width = 1920;
            a.info.video.height = 1080;
            a.info.video.fps = {60, 1};
            for (int k = 0; k <= i % 2; ++k) a.info.audio.push_back({k + 1, 48000, 2});
            p.assets.push_back(a);
        }
        d.reset(p);
    }

    int irand(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    bool coin() { return irand(0, 1) == 1; }
    Tick frames(int lo, int hi) { return Tick(irand(lo, hi)) * F; }
    const Sequence& seq() const { return d.project.sequence; }
    Id randomClip() {
        if (seq().clips.empty()) return 0;
        return seq().clips[size_t(irand(0, int(seq().clips.size()) - 1))].id;
    }

    // one random operation inside an undo transaction
    // clips on locked tracks before the last operation (they must not change)
    std::map<Id, std::vector<Clip>> lockedBefore;
    std::map<Id, std::vector<Clip>> lockedClips() const {
        std::map<Id, std::vector<Clip>> m;
        for (const auto& t : seq().tracks)
            if (t.locked) {
                auto& v = m[t.id];
                for (const auto& c : seq().clips)
                    if (c.trackId == t.id) v.push_back(c);
                std::sort(v.begin(), v.end(), [](const Clip& a, const Clip& b) { return a.id < b.id; });
            }
        return m;
    }

    void step() {
        lockedBefore = lockedClips();
        const int op = irand(0, 33);
        const Id cid = randomClip();
        const Tick t = frames(0, 60 * 40);
        const Asset& a = d.project.assets[size_t(irand(0, 2))];
        auto run = [&](const char* name, auto&& fn) {
            lastOp = name;
            if (std::string(name) != "speed" && std::string(name) != "trim") lastDetail.clear();
            d.edit(name, fn);
        };
        switch (op) {
        case 0:
        case 1: {
            const Tick len = std::max<Tick>(F, frames(1, int(a.info.duration / F)));
            const Tick in = frames(0, int((a.info.duration - len) / F));
            run(op == 0 ? "overwrite" : "insert", [&](Project& p) {
                auto clips = edit::clipsForAsset(p.sequence, a, t, in, in + len, irand(0, 1), irand(0, 1));
                if (op == 0) edit::overwriteClips(p.sequence, clips);
                else edit::insertClips(p.sequence, clips, t);
            });
            break;
        }
        case 2:
            if (const Clip* c = seq().clip(cid); c && c->duration > 2 * F) {
                const Tick at = c->start + frames(1, int(c->duration / F) - 1);
                run("razor", [&](Project& p) { edit::razor(p.sequence, cid, at, coin()); });
            }
            break;
        case 3: run("add edit", [&](Project& p) { edit::addEdit(p.sequence, t, {}); }); break;
        case 4: if (cid) run("delete", [&](Project& p) { edit::deleteClips(p.sequence, {cid}); }); break;
        case 5: if (cid) run("ripple delete", [&](Project& p) { edit::rippleDelete(p.sequence, p.sequence.linkedClips(cid)); }); break;
        case 6:
            if (cid) {
                const Tick dt = frames(-300, 300);
                const int dv = irand(-1, 1), da = irand(-1, 1);
                const bool ins = coin();
                run("move", [&](Project& p) { edit::moveClips(p.sequence, p.sequence.linkedClips(cid), dt, dv, da, ins); });
            }
            break;
        case 7:
            if (const Clip* c = seq().clip(cid)) {
                const edit::Edge e = coin() ? edit::Edge::Head : edit::Edge::Tail;
                const Tick pos = (e == edit::Edge::Head ? c->start : c->end()) + frames(-200, 200);
                const bool ripple = coin(), alone = coin();
                lastDetail = "clip " + std::to_string(cid) + (e == edit::Edge::Head ? " head" : " tail") + " to " + std::to_string(pos) +
                             (ripple ? " ripple" : "") + (alone ? " alone" : "") + " (start " + std::to_string(c->start) + " end " +
                             std::to_string(c->end()) + " link " + std::to_string(c->linkId) + ")";
                run("trim", [&](Project& p) { edit::trimClip(p.sequence, p.assets, cid, e, std::max<Tick>(0, pos), ripple, alone); });
            }
            break;
        case 8:
            if (cid) {
                const double sp = std::uniform_real_distribution<double>(0.25, 4.0)(rng);
                const bool rev = coin(), ripple = coin();
                lastDetail = "clip " + std::to_string(cid) + " speed " + std::to_string(sp) + (ripple ? " ripple" : "");
                if (const Clip* pc = seq().clip(cid))
                    lastDetail += " (was speed " + std::to_string(pc->speed) + " dur " + std::to_string(pc->duration) + " in " +
                                  std::to_string(pc->sourceIn) + ")";
                run("speed", [&](Project& p) { edit::setSpeed(p.sequence, p.assets, cid, sp, rev, ripple, coin()); });
            }
            break;
        case 9:
            if (const Clip* c = seq().clip(cid); c && c->kind == ClipKind::Media && c->duration > 2 * F) {
                const Tick at = c->start + frames(1, int(c->duration / F) - 1);
                run("frame hold", [&](Project& p) { edit::addFrameHold(p.sequence, cid, at); });
            }
            break;
        case 10: {
            // a transition at a random cut (adjacent clips on one track)
            std::vector<std::pair<const Clip*, const Clip*>> cuts;
            for (const auto& x : seq().clips)
                for (const auto& y : seq().clips)
                    if (x.trackId == y.trackId && x.end() == y.start && x.id != y.id) cuts.push_back({&x, &y});
            if (!cuts.empty()) {
                auto [x, y] = cuts[size_t(irand(0, int(cuts.size()) - 1))];
                const Id tr = x->trackId, xa = x->id, yb = y->id;
                const Tick dur = frames(2, 90);
                const auto align = TransitionAlign(irand(0, 2));
                run("transition", [&](Project& p) {
                    const bool audio = p.sequence.track(tr)->kind == TrackKind::Audio;
                    edit::addTransition(p.sequence, tr, xa, yb, audio ? TransitionType::ConstantPower : TransitionType::CrossDissolve,
                                        dur, align);
                });
            }
            break;
        }
        case 11: run("shift left", [&](Project& p) { edit::rippleShiftLeft(p.sequence, t, frames(1, 300)); }); break;
        case 12: run("insert space", [&](Project& p) { edit::insertSpace(p.sequence, t, frames(1, 300)); }); break;
        case 13:
            if (cid) {
                const Tick dt = frames(-200, 200);
                run("duplicate", [&](Project& p) { edit::duplicateClips(p.sequence, p.sequence.linkedClips(cid), dt, irand(0, 1), irand(0, 1)); });
            }
            break;
        case 14:
            if (cid) run("unlink", [&](Project& p) { edit::unlinkClips(p.sequence, p.sequence.linkedClips(cid)); });
            break;
        case 16: {
            const Tick len = frames(10, 240);
            run("caption", [&](Project& p) { edit::addCaption(p.sequence, t, t + len, "fuzz"); });
            break;
        }
        case 17:
            if (!seq().captions.empty()) {
                const Caption& cap = seq().captions[size_t(irand(0, int(seq().captions.size()) - 1))];
                const Id id = cap.id;
                const int what = irand(0, 2);
                const Tick at = cap.start + (cap.end - cap.start) / 2;
                run("caption edit", [&](Project& p) {
                    if (what == 0) edit::splitCaption(p.sequence, id, p.sequence.frameDur() * (at / p.sequence.frameDur()));
                    else if (what == 1) edit::mergeCaptionWithNext(p.sequence, id);
                    else edit::deleteCaptions(p.sequence, {id});
                });
            }
            break;
        case 18: {
            const int vt = irand(0, 2);
            const Tick len = frames(30, 300);
            run("title", [&](Project& p) {
                const Track* tr = p.sequence.trackAt(TrackKind::Video, vt);
                if (!tr) return;
                const int k = irand(0, 2);
                edit::overwriteClips(p.sequence, {k == 0 ? makeTextClip(tr->id, t, len, "Title")
                                                  : k == 1 ? makeMatteClip(tr->id, t, len, 0xFF336699)
                                                           : makeAdjustmentClip(tr->id, t, len)});
            });
            break;
        }
        case 19:
            if (const Clip* c = seq().clip(cid); c && c->kind == ClipKind::Media && c->duration > 2 * F) {
                const Tick at = c->start + frames(1, int(c->duration / F) - 1);
                const Tick len = frames(1, 120);
                run("hold segment", [&](Project& p) { edit::insertFrameHoldSegment(p.sequence, cid, at, len); });
            }
            break;
        case 20: {
            const Id other = randomClip();
            if (cid && other && cid != other) run("link", [&](Project& p) { edit::linkClips(p.sequence, {cid, other}); });
            break;
        }
        case 21: {
            const bool add = coin();
            const TrackKind k = coin() ? TrackKind::Video : TrackKind::Audio;
            const auto tracks = seq().tracksOf(k);
            const Id victim = tracks.empty() ? 0 : tracks[size_t(irand(0, int(tracks.size()) - 1))]->id;
            run(add ? "add track" : "delete track", [&](Project& p) {
                if (add) edit::addTrack(p.sequence, k);
                else if (victim) edit::deleteTrack(p.sequence, victim);
            });
            break;
        }
        case 22:
        case 23: {
            // lift (clear a range on every track) or extract (and close it)
            const Tick len = frames(1, 300);
            const bool extract = op == 23;
            run(extract ? "extract" : "lift", [&](Project& p) {
                for (const auto& tr : p.sequence.tracks)
                    if (tr.kind != TrackKind::Caption && !tr.locked) edit::clearRange(p.sequence, tr.id, t, t + len);
                if (extract) edit::rippleShiftLeft(p.sequence, t, len);
            });
            break;
        }
        case 24: {
            // lock / unlock a track: every edit must leave locked tracks alone and stay valid
            const auto& tracks = seq().tracks;
            const Id tr = tracks[size_t(irand(0, int(tracks.size()) - 1))].id;
            run("lock", [&](Project& p) {
                if (Track* x = p.sequence.track(tr)) x->locked = !x->locked;
            });
            break;
        }
        case 25:
            run("seq new", [&](Project& p) {
                Sequence s = coin() ? p.sequence : makeDefaultSequence({1920, 1080, {60, 1}, 48000});
                s.name = "Seq " + std::to_string(irand(0, 999));
                edit::insertSequence(p, irand(0, edit::sequenceCount(p)), std::move(s));
            });
            break;
        case 26: {
            const int tab = irand(0, edit::sequenceCount(d.project) - 1);
            run("seq switch", [&](Project& p) { edit::switchSequence(p, tab); });
            break;
        }
        case 27: {
            const int tab = irand(0, edit::sequenceCount(d.project) - 1);
            if (coin()) run("seq delete", [&](Project& p) { edit::removeSequence(p, tab); });
            else run("seq rename", [&](Project& p) { edit::renameSequence(p, tab, "Renamed"); });
            break;
        }
        case 28:
            if (cid) {
                const Id other = randomClip();
                run("nest", [&](Project& p) {
                    std::vector<Id> ids = p.sequence.linkedClips(cid);
                    if (other && other != cid) ids.push_back(other);
                    edit::nestClips(p, ids, "Nested");
                });
            }
            break;
        case 29:
            if (const Clip* c = seq().clip(cid); c && c->duration > 60 * F) {
                const Tick at = c->start + frames(1, int(c->duration / F) - 1);
                const double sl = std::uniform_real_distribution<double>(0.15, 0.9)(rng);
                const Tick slowLen = frames(4, 90), rampLen = frames(2, 40);
                const int steps = irand(1, 8);
                run("speed ramp", [&](Project& p) { edit::speedRamp(p.sequence, p.assets, cid, at, sl, slowLen, rampLen, steps); });
            }
            break;
        case 33: {
            // to another rate and back: the model must stay valid at every step
            static const Rational rates[] = {{24, 1}, {25, 1}, {30, 1}, {50, 1}, {30000, 1001}, {60000, 1001}, {120, 1}};
            const Rational r = rates[irand(0, 6)];
            run("frame rate", [&](Project& p) {
                edit::changeFrameRate(p.sequence, p.assets, r);
                edit::changeFrameRate(p.sequence, p.assets, {60, 1});
            });
            break;
        }
        case 32:
            if (cid) {
                const Tick in = frames(0, int(a.info.duration / F));
                run("replace media", [&](Project& p) { edit::replaceMedia(p.sequence, a, {cid}, in); });
            }
            break;
        case 31:
            if (cid) {
                const Tick end = frames(1, 1500), fade = frames(0, 200);
                run("fit music", [&](Project& p) { edit::fitAudioToEnd(p.sequence, p.assets, cid, end, fade); });
            }
            break;
        case 30: {
            const Tick at = frames(0, 900), len = frames(1, 300);
            const double sp = std::uniform_real_distribution<double>(0.2, 1.0)(rng);
            run("instant replay", [&](Project& p) { edit::instantReplay(p.sequence, at, len, sp); });
            break;
        }
        case 15: {
            Tick ga = 0, gb = 0;
            const auto& tracks = seq().tracks;
            const Id tr = tracks[size_t(irand(0, int(tracks.size()) - 1))].id;
            if (edit::gapAt(seq(), tr, t, ga, gb)) run("ripple gap", [&](Project& p) { edit::rippleDeleteGap(p.sequence, tr, t); });
            break;
        }
        }
    }

    // returns a description of the first broken invariant (empty = fine)
    std::string check() const {
        std::string r = checkSeq(seq(), true);
        for (const auto& o : d.project.otherSequences)
            if (r.empty()) r = checkSeq(o, false);
        // sequence identities are unique
        if (r.empty()) {
            std::set<Id> uids{seq().uid};
            for (const auto& o : d.project.otherSequences)
                if (!uids.insert(o.uid).second || o.uid == 0) r = "duplicate or missing sequence uid";
        }
        if (r.empty() && (d.project.activeSequence < 0 || d.project.activeSequence > int(d.project.otherSequences.size())))
            r = "active sequence index out of range";
        return r;
    }

    std::string checkSeq(const Sequence& s, bool active) const {
        std::ostringstream err;
        std::map<Id, std::vector<const Clip*>> byTrack;
        std::set<Id> seenIds;
        for (const auto& c : s.clips) {
            if (!seenIds.insert(c.id).second || c.id == 0) return "duplicate or missing clip id " + std::to_string(c.id);
            const Track* tr = s.track(c.trackId);
            if (!tr) return "clip " + std::to_string(c.id) + " on a missing track";
            if (c.duration <= 0) return "clip " + std::to_string(c.id) + " has no duration";
            if (c.start < 0) return "clip " + std::to_string(c.id) + " starts before 0";
            if (c.start % F || c.duration % F) return "clip " + std::to_string(c.id) + " not frame aligned";
            if (c.kind == ClipKind::Nested && c.nestedSeq == s.uid) return "sequence " + s.name + " nests itself";
            if (c.kind == ClipKind::Media) {
                const bool audioTrack = tr->kind == TrackKind::Audio;
                if (audioTrack == c.volume.type.empty()) return "clip " + std::to_string(c.id) + " kind does not match its track";
                const Asset* a = d.project.asset(c.assetId);
                if (!a) return "clip on a missing asset";
                if (!c.isHold() && (c.sourceIn < 0 || c.sourceOut() > a->info.duration + F))
                    return "clip " + std::to_string(c.id) + " source range [" + std::to_string(c.sourceIn) + ", " +
                           std::to_string(c.sourceOut()) + ") outside media " + std::to_string(a->info.duration) + " (speed " +
                           std::to_string(c.speed) + " dur " + std::to_string(c.duration) + " link " + std::to_string(c.linkId) +
                           (c.reverse ? " reverse" : "") + ")";
            }
            byTrack[c.trackId].push_back(&c);
        }
        for (auto& [tid, cs] : byTrack) {
            std::sort(cs.begin(), cs.end(), [](const Clip* x, const Clip* y) { return x->start < y->start; });
            for (size_t i = 1; i < cs.size(); ++i)
                if (cs[i - 1]->end() > cs[i]->start)
                    return "clips " + std::to_string(cs[i - 1]->id) + " and " + std::to_string(cs[i]->id) + " overlap";
        }
        for (size_t i = 0; i < s.captions.size(); ++i) {
            const Caption& cp = s.captions[i];
            if (cp.end <= cp.start) return "caption " + std::to_string(cp.id) + " is empty";
            if (i > 0 && s.captions[i - 1].start > cp.start) return "captions are unsorted";  // (overlaps are allowed: SRT)
        }
        for (const auto& tr : s.transitions) {
            const Clip* a = tr.clipA ? s.clip(tr.clipA) : nullptr;
            const Clip* b = tr.clipB ? s.clip(tr.clipB) : nullptr;
            if ((tr.clipA && !a) || (tr.clipB && !b)) return "transition on a missing clip";
            if (a && a->trackId != tr.trackId) return "transition clip A on another track";
            if (b && b->trackId != tr.trackId) return "transition clip B on another track";
            if (a && b && a->end() != b->start) return "transition between clips that do not touch";
            if (tr.duration <= 0) return "transition without duration";
        }
        if (active && lastOp != "lock" && lastOp != "delete track" && lastOp != "frame rate" && lastOp.rfind("seq ", 0) != 0) {  // (a new frame rate conforms every track)
            const auto now = lockedClips();
            for (const auto& [tid, clips] : lockedBefore) {
                auto it = now.find(tid);
                if (it != now.end() && !(it->second == clips)) return "a locked track changed";
            }
        }
        return {};
    }
};

std::string describeDiff(const Sequence& a, const Sequence& b) {
    std::ostringstream o;
    if (a.clips.size() != b.clips.size()) o << "clip count " << a.clips.size() << " vs " << b.clips.size() << "; ";
    for (size_t i = 0; i < std::min(a.clips.size(), b.clips.size()); ++i)
        if (!(a.clips[i] == b.clips[i])) {
            const Clip &x = a.clips[i], &y = b.clips[i];
            o << "clip[" << i << "] id " << x.id << "/" << y.id << " start " << x.start << "/" << y.start << " dur " << x.duration << "/"
              << y.duration << " in " << x.sourceIn << "/" << y.sourceIn << " speed " << x.speed << "/" << y.speed << " link " << x.linkId
              << "/" << y.linkId << " track " << x.trackId << "/" << y.trackId << " effects " << x.effects.size() << "/" << y.effects.size()
              << " motionEq " << (x.motion == y.motion) << " volEq " << (x.volume == y.volume) << "; ";
            break;
        }
    if (!(a.tracks == b.tracks)) o << "tracks differ; ";
    if (!(a.transitions == b.transitions)) o << "transitions " << a.transitions.size() << " vs " << b.transitions.size() << "; ";
    if (!(a.markers == b.markers)) o << "markers differ; ";
    if (!(a.captions == b.captions)) o << "captions differ; ";
    if (a.nextId != b.nextId) o << "nextId " << a.nextId << " vs " << b.nextId << "; ";
    return o.str();
}

} // namespace

TEST_CASE("fuzz: random edits keep the model valid and undo/redo exact") {
    // CUTLINE_FUZZ_SEEDS=N runs a longer campaign (default 12 seeds keeps the unit run quick)
    unsigned seeds = 12;
    if (const char* e = std::getenv("CUTLINE_FUZZ_SEEDS")) seeds = unsigned(std::max(1, std::atoi(e)));
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        Fuzzer f(seed);
        struct State {
            Sequence active;
            std::vector<Sequence> others;
            int index;
            bool operator==(const State&) const = default;
        };
        auto snap = [&] { return State{f.seq(), f.d.project.otherSequences, f.d.project.activeSequence}; };
        std::vector<State> states{snap()};
        for (int i = 0; i < 400; ++i) {
            const size_t before = f.d.undo.size();
            f.step();
            if (f.d.undo.size() != before) states.push_back(snap());
            const std::string bad = f.check();
            if (!bad.empty()) {
                FAIL_CHECK("seed " << seed << " step " << i << " after '" << f.lastOp << "' [" << f.lastDetail << "]: " << bad);
                break;
            }
        }
        // the random project survives save -> load -> save unchanged
        {
            const std::string json = serializeProject(f.d.project, "C:/fuzz/project.cutproj");
            Project back;
            std::string err;
            REQUIRE(deserializeProject(json, back, err));
            if (!(back.sequence == f.seq() && back.otherSequences == f.d.project.otherSequences && back.activeSequence == f.d.project.activeSequence)) FAIL_CHECK("seed " << seed << ": project load differs: " << describeDiff(back.sequence, f.seq()));
            if (serializeProject(back, "C:/fuzz/project.cutproj") != json) FAIL_CHECK("seed " << seed << ": re-save is not byte-identical");
        }
        // undo everything, then redo everything: each intermediate state must come back exactly
        for (size_t i = states.size() - 1; i > 0; --i) {
            REQUIRE(f.d.doUndo());
            if (!(snap() == states[i - 1])) {
                FAIL_CHECK("seed " << seed << ": undo to state " << i - 1 << " differs: " << describeDiff(f.seq(), states[i - 1].active));
                break;
            }
        }
        for (size_t i = 1; i < states.size(); ++i) {
            REQUIRE(f.d.doRedo());
            if (!(snap() == states[i])) {
                FAIL_CHECK("seed " << seed << ": redo to state " << i << " differs: " << describeDiff(f.seq(), states[i].active));
                break;
            }
        }
    }
}
