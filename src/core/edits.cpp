#include "core/edits.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

namespace cut::edit {

namespace {

constexpr Tick kNegInf = INT64_MIN / 4;
constexpr Tick kPosInf = INT64_MAX / 4;

template <class F>
void forEachParam(Clip& c, F&& f) {
    auto visit = [&](Effect& e) {
        for (auto& [k, p] : e.params) f(p);
    };
    visit(c.motion);
    visit(c.opacity);
    visit(c.volume);
    for (auto& e : c.effects) visit(e);
}

void splitParam(Param& left, Param& right, Tick d) {
    if (left.keys.empty()) return;
    const Param orig = left;
    double vd = orig.at(d);
    left.keys.erase(std::remove_if(left.keys.begin(), left.keys.end(), [&](const Keyframe& k) { return k.t > d; }),
                    left.keys.end());
    if (orig.keys.back().t > d && (left.keys.empty() || left.keys.back().t != d))
        left.keys.push_back({d, vd, Interp::Linear});
    right.keys.clear();
    bool hadBefore = false;
    for (const auto& k : orig.keys) {
        if (k.t < d) {
            hadBefore = true;
            continue;
        }
        right.keys.push_back({k.t - d, k.v, k.interp});
    }
    if (hadBefore && (right.keys.empty() || right.keys.front().t != 0))
        right.keys.insert(right.keys.begin(), {0, vd, Interp::Linear});
    if (right.keys.empty()) right.value = vd;
}

void splitAllParams(Clip& left, Clip& right, Tick d) {
    std::vector<Param*> lp, rp;
    forEachParam(left, [&](Param& p) { lp.push_back(&p); });
    forEachParam(right, [&](Param& p) { rp.push_back(&p); });
    for (size_t i = 0; i < lp.size() && i < rp.size(); ++i) splitParam(*lp[i], *rp[i], d);
}

Tick spanFor(const Clip& c, Tick dur) {
    if (c.isHold()) return 0;
    if (c.speed == 1.0) return dur;
    return Tick(std::llround(double(dur) * c.speed));
}

// Moves the clip head to newStart (may extend or shorten). Source/keys follow content.
void trimHeadTo(Clip& c, Tick newStart) {
    Tick delta = newStart - c.start;
    if (!c.isHold() && !c.reverse) {
        Tick off = c.speed == 1.0 ? delta : Tick(std::llround(double(delta) * c.speed));
        c.sourceIn += off;
    }
    c.start = newStart;
    c.duration -= delta;
    shiftKeys(c, delta);
    c.fadeIn = std::min(c.fadeIn, c.duration);
    c.fadeOut = std::min(c.fadeOut, c.duration);
}

void trimTailTo(Clip& c, Tick newEnd) {
    Tick oldSpan = c.sourceSpan();
    c.duration = newEnd - c.start;
    if (c.reverse && !c.isHold()) c.sourceIn += oldSpan - c.sourceSpan();
    c.fadeIn = std::min(c.fadeIn, c.duration);
    c.fadeOut = std::min(c.fadeOut, c.duration);
}

bool trackUnlocked(const Sequence& s, Id trackId, const std::set<Id>& only) {
    const Track* t = s.track(trackId);
    if (!t || t->locked) return false;
    return only.empty() || only.count(trackId);
}

Tick ceilFrame(Tick t, Rational fps) {
    Tick f = floorToFrame(t, fps);
    return f < t ? frameToTicks(ticksToFrame(t, fps) + 1, fps) : f;
}

// Shift items with start >= from right by amount (no splitting).
void shiftRight(Sequence& s, Tick from, Tick amount, const std::set<Id>& only, const std::set<Id>& except) {
    for (auto& c : s.clips)
        if (c.start >= from && !except.count(c.id) && trackUnlocked(s, c.trackId, only)) c.start += amount;
    for (const auto& t : s.tracks)
        if (t.kind == TrackKind::Caption && trackUnlocked(s, t.id, only))
            for (auto& cap : s.captions)
                if (cap.start >= from) {
                    cap.start += amount;
                    cap.end += amount;
                }
}

} // namespace

void shiftKeys(Clip& c, Tick delta) {
    if (!delta) return;
    forEachParam(c, [&](Param& p) {
        for (auto& k : p.keys) k.t -= delta;
    });
}

Tick mediaLimit(const std::vector<Asset>& assets, const Clip& c) {
    if (c.kind != ClipKind::Media) return 0;  // titles and nested sequences extend freely
    for (const auto& a : assets)
        if (a.id == c.assetId) {
            if (a.info.isImage) return 0;
            return a.info.duration;
        }
    return 0;
}

void normalize(Sequence& s) {
    s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(),
                                 [&](const Clip& c) { return c.duration <= 0 || !s.track(c.trackId); }),
                  s.clips.end());
    std::sort(s.clips.begin(), s.clips.end(), [](const Clip& a, const Clip& b) { return a.id < b.id; });

    const Tick fd = s.frameDur();
    std::vector<Transition> keep;
    std::set<std::pair<Id, Id>> seen;
    for (auto it = s.transitions.rbegin(); it != s.transitions.rend(); ++it) {
        Transition t = *it;
        const Clip* a = t.clipA ? s.clip(t.clipA) : nullptr;
        const Clip* b = t.clipB ? s.clip(t.clipB) : nullptr;
        if ((t.clipA && !a) || (t.clipB && !b) || (!a && !b)) continue;
        if (a && b) {
            if (a->trackId != b->trackId || a->end() != b->start) {
                // No longer adjacent: keep as a one-sided transition only if it was never two-sided.
                continue;
            }
            t.trackId = a->trackId;
            t.duration = std::min({t.duration, a->duration + (t.align == TransitionAlign::End ? 0 : b->duration),
                                   std::max(a->duration, b->duration) * 2});
            if (t.align == TransitionAlign::Start) t.duration = std::min(t.duration, b->duration);
            if (t.align == TransitionAlign::End) t.duration = std::min(t.duration, a->duration);
            if (t.align == TransitionAlign::Center)
                t.duration = std::min(t.duration, 2 * std::min(a->duration, b->duration));
        } else {
            const Clip* c = a ? a : b;
            t.trackId = c->trackId;
            t.duration = std::min(t.duration, c->duration);
        }
        t.duration = std::max(t.duration, fd);
        if (!seen.insert({t.clipA, t.clipB}).second) continue;
        keep.push_back(t);
    }
    std::sort(keep.begin(), keep.end(), [](const Transition& a, const Transition& b) { return a.id < b.id; });
    s.transitions = std::move(keep);
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    sortCaptions(s);
}

Id splitClip(Sequence& s, Id clipId, Tick t, Id newLinkId) {
    Clip* c = s.clip(clipId);
    if (!c || t <= c->start || t >= c->end()) return 0;
    Clip left = *c, right = *c;
    Tick d = t - c->start;
    left.duration = d;
    right.start = t;
    right.duration = c->duration - d;
    if (!c->isHold()) {
        Tick spanLeft = spanFor(*c, d);
        if (!c->reverse) right.sourceIn = c->sourceIn + spanLeft;
        else {
            left.sourceIn = c->sourceOut() - spanLeft;
            right.sourceIn = c->sourceIn;
        }
    }
    splitAllParams(left, right, d);
    left.fadeOut = 0;
    right.fadeIn = 0;
    right.id = s.newId();
    right.linkId = c->linkId ? newLinkId : 0;
    for (auto& tr : s.transitions)
        if (tr.clipA == clipId) tr.clipA = right.id;
    *c = left;
    Id rid = right.id;
    s.clips.push_back(std::move(right));
    return rid;
}

std::vector<Id> razor(Sequence& s, Id clipId, Tick t, bool alone) {
    std::vector<Id> ids = alone ? std::vector<Id>{clipId} : s.linkedClips(clipId);
    const Clip* c = s.clip(clipId);
    Id newLink = (c && c->linkId && ids.size() > 1) ? s.newId() : 0;
    std::vector<Id> out;
    for (Id id : ids) {
        const Clip* x = s.clip(id);
        if (!x || !x->contains(t) || t == x->start) continue;
        const Track* tr = s.track(x->trackId);
        if (tr && tr->locked) continue;
        if (Id r = splitClip(s, id, t, newLink)) out.push_back(r);
    }
    return out;
}

void addEdit(Sequence& s, Tick t, const std::vector<Id>& only) {
    std::vector<Id> targets;
    if (only.empty()) {
        for (const auto& c : s.clips) {
            const Track* tr = s.track(c.trackId);
            if (tr && !tr->locked && c.start < t && t < c.end()) targets.push_back(c.id);
        }
    } else targets = only;
    std::map<Id, Id> linkMap;
    for (Id id : targets) {
        const Clip* c = s.clip(id);
        if (!c || !(c->start < t && t < c->end())) continue;
        Id nl = 0;
        if (c->linkId) {
            auto it = linkMap.find(c->linkId);
            if (it == linkMap.end()) it = linkMap.emplace(c->linkId, s.newId()).first;
            nl = it->second;
        }
        splitClip(s, id, t, nl);
    }
}

void clearRange(Sequence& s, Id trackId, Tick a, Tick b, const std::set<Id>& except) {
    if (b <= a) return;
    std::vector<Id> ids;
    for (const auto& c : s.clips)
        if (c.trackId == trackId && c.start < b && c.end() > a && !except.count(c.id)) ids.push_back(c.id);
    for (Id id : ids) {
        Clip* c = s.clip(id);
        if (!c) continue;
        if (c->start >= a && c->end() <= b) {
            s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(), [&](const Clip& x) { return x.id == id; }),
                          s.clips.end());
        } else if (c->start < a && c->end() > b) {
            Id r = splitClip(s, id, b, 0);
            if (Clip* rc = s.clip(r)) rc->linkId = 0;
            c = s.clip(id);
            trimTailTo(*c, a);
        } else if (c->start >= a) {
            trimHeadTo(*c, b);
        } else {
            trimTailTo(*c, a);
        }
    }
}

void deleteClips(Sequence& s, const std::vector<Id>& ids) {
    std::set<Id> del;
    for (Id id : ids) {
        const Clip* c = s.clip(id);
        if (!c) continue;
        const Track* t = s.track(c->trackId);
        if (t && t->locked) continue;
        del.insert(id);
    }
    s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(), [&](const Clip& c) { return del.count(c.id) > 0; }),
                  s.clips.end());
}

Tick rippleShiftLeft(Sequence& s, Tick from, Tick amount, const std::set<Id>& only) {
    if (amount <= 0) return 0;
    Tick allowed = amount;
    for (const auto& tr : s.tracks) {
        if (!trackUnlocked(s, tr.id, only)) continue;
        Tick firstMoved = kPosInf, staticEnd = 0;
        if (tr.kind == TrackKind::Caption) {
            for (const auto& c : s.captions) {
                if (c.start >= from) firstMoved = std::min(firstMoved, c.start);
                else staticEnd = std::max(staticEnd, c.end);
            }
        } else {
            for (const auto& c : s.clips) {
                if (c.trackId != tr.id) continue;
                if (c.start >= from) firstMoved = std::min(firstMoved, c.start);
                else staticEnd = std::max(staticEnd, c.end());
            }
        }
        if (firstMoved == kPosInf) continue;
        allowed = std::min(allowed, firstMoved - staticEnd);
    }
    allowed = std::max<Tick>(0, allowed);
    if (!allowed) return 0;
    for (auto& c : s.clips)
        if (c.start >= from && trackUnlocked(s, c.trackId, only)) c.start -= allowed;
    for (const auto& tr : s.tracks)
        if (tr.kind == TrackKind::Caption && trackUnlocked(s, tr.id, only))
            for (auto& c : s.captions)
                if (c.start >= from) {
                    c.start -= allowed;
                    c.end -= allowed;
                }
    return allowed;
}

void insertSpace(Sequence& s, Tick from, Tick amount, const std::set<Id>& only) {
    if (amount <= 0) return;
    std::map<Id, Id> linkMap;
    std::vector<Id> crossing;
    for (const auto& c : s.clips)
        if (c.start < from && from < c.end() && trackUnlocked(s, c.trackId, only)) crossing.push_back(c.id);
    for (Id id : crossing) {
        const Clip* c = s.clip(id);
        Id nl = 0;
        if (c->linkId) {
            auto it = linkMap.find(c->linkId);
            if (it == linkMap.end()) it = linkMap.emplace(c->linkId, s.newId()).first;
            nl = it->second;
        }
        splitClip(s, id, from, nl);
    }
    shiftRight(s, from, amount, only, {});
}

void rippleDelete(Sequence& s, const std::vector<Id>& ids) {
    Tick a = kPosInf, b = kNegInf;
    std::vector<Id> del;
    for (Id id : ids) {
        const Clip* c = s.clip(id);
        if (!c) continue;
        const Track* t = s.track(c->trackId);
        if (t && t->locked) continue;
        a = std::min(a, c->start);
        b = std::max(b, c->end());
        del.push_back(id);
    }
    if (del.empty()) return;
    deleteClips(s, del);
    rippleShiftLeft(s, a, b - a);
}

bool gapAt(const Sequence& s, Id trackId, Tick t, Tick& a, Tick& b) {
    a = 0;
    b = kPosInf;
    for (const auto& c : s.clips) {
        if (c.trackId != trackId) continue;
        if (c.contains(t)) return false;
        if (c.end() <= t) a = std::max(a, c.end());
        if (c.start > t) b = std::min(b, c.start);
    }
    return b != kPosInf && b > a;
}

bool rippleDeleteGap(Sequence& s, Id trackId, Tick t) {
    Tick a, b;
    if (!gapAt(s, trackId, t, a, b)) return false;
    return rippleShiftLeft(s, b, b - a) > 0;
}

namespace {
bool targetTrack(const Sequence& s, const Clip& c, int dV, int dA, Id& out) {
    const Track* tr = s.track(c.trackId);
    if (!tr) return false;
    int idx = s.trackIndexInKind(c.trackId);
    int d = tr->kind == TrackKind::Video ? dV : tr->kind == TrackKind::Audio ? dA : 0;
    const Track* t = s.trackAt(tr->kind, idx + d);
    if (!t || t->locked) return false;
    out = t->id;
    return true;
}

MoveResult place(Sequence& s, std::vector<Clip> moving, Tick dt, int dV, int dA, bool insert, bool removeOriginals) {
    MoveResult r;
    if (moving.empty()) return r;
    Tick minStart = kPosInf;
    for (const auto& c : moving) minStart = std::min(minStart, c.start);
    if (minStart + dt < 0) dt = -minStart;
    std::vector<Id> targets;
    for (const auto& c : moving) {
        Id t;
        if (!targetTrack(s, c, dV, dA, t)) return r;
        targets.push_back(t);
    }
    if (removeOriginals) {
        std::set<Id> ids;
        for (const auto& c : moving) ids.insert(c.id);
        s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(), [&](const Clip& c) { return ids.count(c.id) > 0; }),
                      s.clips.end());
    }
    if (insert) {
        Tick a = kPosInf, b = kNegInf;
        for (const auto& c : moving) {
            a = std::min(a, c.start + dt);
            b = std::max(b, c.end() + dt);
        }
        insertSpace(s, a, b - a);
    } else {
        for (size_t i = 0; i < moving.size(); ++i)
            clearRange(s, targets[i], moving[i].start + dt, moving[i].end() + dt);
    }
    for (size_t i = 0; i < moving.size(); ++i) {
        Clip c = moving[i];
        c.start += dt;
        c.trackId = targets[i];
        r.moved.push_back(c.id);
        s.clips.push_back(std::move(c));
    }
    r.ok = true;
    return r;
}
} // namespace

MoveResult moveClips(Sequence& s, const std::vector<Id>& ids, Tick dt, int dV, int dA, bool insert) {
    std::vector<Clip> moving;
    for (Id id : ids) {
        const Clip* c = s.clip(id);
        if (!c) continue;
        const Track* t = s.track(c->trackId);
        if (t && t->locked) continue;
        moving.push_back(*c);
    }
    return place(s, std::move(moving), dt, dV, dA, insert, true);
}

MoveResult duplicateClips(Sequence& s, const std::vector<Id>& ids, Tick dt, int dV, int dA) {
    std::vector<Clip> copies;
    std::map<Id, Id> linkMap;
    for (Id id : ids) {
        const Clip* c = s.clip(id);
        if (!c) continue;
        Clip x = *c;
        x.id = s.newId();
        if (x.linkId) {
            auto it = linkMap.find(x.linkId);
            if (it == linkMap.end()) it = linkMap.emplace(x.linkId, s.newId()).first;
            x.linkId = it->second;
        }
        copies.push_back(std::move(x));
    }
    return place(s, std::move(copies), dt, dV, dA, false, false);
}

TickRange trimRange(const Sequence& s, const std::vector<Asset>& assets, const Clip& c, Edge e, bool ripple) {
    const Rational fps = s.settings.fps;
    const Tick fd = s.frameDur();
    const Tick limit = mediaLimit(assets, c);
    Tick prevEnd = 0, nextStart = kPosInf;
    for (const auto& o : s.clips) {
        if (o.trackId != c.trackId || o.id == c.id) continue;
        if (o.start < c.start) prevEnd = std::max(prevEnd, o.end());
        else nextStart = std::min(nextStart, o.start);
    }
    const double sp = c.isHold() ? 1.0 : c.speed;
    if (e == Edge::Head) {
        // For ripple trims the clip start stays put and `pos` is a virtual edge position
        // (start + removed amount), so it may go below zero; otherwise neighbours/zero bound it.
        Tick mn = kNegInf;  // stills, titles and holds extend freely
        if (!c.isHold() && limit > 0) {
            Tick avail = c.reverse ? limit - c.sourceOut() : c.sourceIn;
            mn = c.start - Tick(std::floor(double(std::max<Tick>(0, avail)) / sp));
        }
        if (!ripple) mn = std::max(mn, prevEnd);
        if (mn != kNegInf) mn = ceilFrame(mn, fps);
        return {mn, c.end() - fd};
    }
    Tick mx = kPosInf;
    if (!c.isHold() && limit > 0) {
        Tick avail = c.reverse ? c.sourceIn : limit - c.sourceOut();
        mx = c.end() + Tick(std::floor(double(std::max<Tick>(0, avail)) / sp));
    }
    if (!ripple) mx = std::min(mx, nextStart);
    if (mx != kPosInf) mx = floorToFrame(mx, fps);
    return {c.start + fd, std::max(mx, c.start + fd)};
}

void trimClip(Sequence& s, const std::vector<Asset>& assets, Id clipId, Edge e, Tick pos, bool ripple, bool alone) {
    const Clip* primary = s.clip(clipId);
    if (!primary) return;
    std::vector<Id> ids = alone ? std::vector<Id>{clipId} : s.linkedClips(clipId);
    // Keep only partners whose edge coincides with the primary's (typical linked A/V).
    const Tick edgePos = e == Edge::Head ? primary->start : primary->end();
    std::vector<Id> group;
    for (Id id : ids) {
        const Clip* c = s.clip(id);
        if (!c) continue;
        const Track* t = s.track(c->trackId);
        if (t && t->locked) continue;
        if ((e == Edge::Head ? c->start : c->end()) == edgePos) group.push_back(id);
    }
    if (group.empty()) return;
    // Feasible delta = intersection over the group.
    Tick dmin = kNegInf, dmax = kPosInf;
    for (Id id : group) {
        const Clip* c = s.clip(id);
        TickRange r = trimRange(s, assets, *c, e, ripple);
        Tick cur = e == Edge::Head ? c->start : c->end();
        dmin = std::max(dmin, r.a - cur);
        dmax = std::min(dmax, r.b - cur);
    }
    if (dmin > dmax) return;
    Tick delta = std::clamp(pos - edgePos, dmin, dmax);
    if (!delta) return;
    const Tick oldEnd = primary->end();
    std::set<Id> groupSet(group.begin(), group.end());
    if (!ripple) {
        for (Id id : group) {
            Clip* c = s.clip(id);
            if (e == Edge::Head) trimHeadTo(*c, c->start + delta);
            else trimTailTo(*c, c->end() + delta);
        }
        return;
    }
    if (e == Edge::Head) {
        // delta > 0 removes frames from the head; clip start stays, later material shifts left.
        // Heads are aligned but tails may not be (Alt-trimmed partners): shift from the earliest tail.
        Tick minEnd = oldEnd;
        for (Id id : group) minEnd = std::min(minEnd, s.clip(id)->end());
        if (delta < 0) shiftRight(s, minEnd, -delta, {}, groupSet);
        for (Id id : group) {
            Clip* c = s.clip(id);
            Tick st = c->start;
            trimHeadTo(*c, c->start + delta);
            c->start = st;
        }
        if (delta > 0) rippleShiftLeft(s, minEnd, delta);
    } else {
        if (delta > 0) shiftRight(s, oldEnd, delta, {}, groupSet);
        for (Id id : group) {
            Clip* c = s.clip(id);
            trimTailTo(*c, c->end() + delta);
        }
        if (delta < 0) rippleShiftLeft(s, oldEnd, -delta);
    }
}

std::vector<Id> overwriteClips(Sequence& s, std::vector<Clip> clips) {
    std::vector<Id> ids;
    for (auto& c : clips) {
        if (!c.id) c.id = s.newId();
        const Track* t = s.track(c.trackId);
        if (!t || t->locked) continue;
        clearRange(s, c.trackId, c.start, c.end());
        ids.push_back(c.id);
        s.clips.push_back(std::move(c));
    }
    return ids;
}

std::vector<Id> insertClips(Sequence& s, std::vector<Clip> clips, Tick at) {
    Tick b = at;
    for (const auto& c : clips) b = std::max(b, c.end());
    insertSpace(s, at, b - at);
    std::vector<Id> ids;
    for (auto& c : clips) {
        if (!c.id) c.id = s.newId();
        const Track* t = s.track(c.trackId);
        if (!t || t->locked) continue;
        clearRange(s, c.trackId, c.start, c.end());
        ids.push_back(c.id);
        s.clips.push_back(std::move(c));
    }
    return ids;
}

namespace {
void renumberTracks(Sequence& s) {
    int v = 0, a = 0, c = 0;
    for (auto& t : s.tracks) {
        if (t.kind == TrackKind::Video) t.name = "V" + std::to_string(++v);
        else if (t.kind == TrackKind::Audio) t.name = "A" + std::to_string(++a);
        else t.name = "C" + std::to_string(++c);
    }
}
} // namespace

Id addTrack(Sequence& s, TrackKind k) {
    Track t;
    t.id = s.newId();
    t.kind = k;
    if (k == TrackKind::Caption) t.height = 32;
    // insert after the last track of the same kind (keeps V..., A..., C... grouping)
    auto pos = s.tracks.end();
    for (auto it = s.tracks.begin(); it != s.tracks.end(); ++it)
        if (it->kind == k) pos = it + 1;
    if (pos == s.tracks.end() && k != TrackKind::Caption) {
        // no track of this kind yet: video before audio before caption
        pos = s.tracks.begin();
        for (auto it = s.tracks.begin(); it != s.tracks.end(); ++it)
            if (int(it->kind) < int(k)) pos = it + 1;
    }
    Id id = t.id;
    s.tracks.insert(pos, t);
    renumberTracks(s);
    return id;
}

bool deleteTrack(Sequence& s, Id trackId) {
    const Track* t = s.track(trackId);
    if (!t) return false;
    if (s.tracksOf(t->kind).size() <= 1) return false;
    s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(), [&](const Clip& c) { return c.trackId == trackId; }),
                  s.clips.end());
    s.tracks.erase(std::remove_if(s.tracks.begin(), s.tracks.end(), [&](const Track& x) { return x.id == trackId; }),
                   s.tracks.end());
    renumberTracks(s);
    return true;
}

std::vector<Id> ensureTracks(Sequence& s, TrackKind k, int n) {
    while (int(s.tracksOf(k).size()) < n) addTrack(s, k);
    std::vector<Id> ids;
    for (const Track* t : s.tracksOf(k)) ids.push_back(t->id);
    return ids;
}

std::vector<Clip> clipsForAsset(Sequence& s, const Asset& a, Tick at, Tick srcIn, Tick srcOut, int vTrack,
                                int aTrack, bool video, bool audio) {
    std::vector<Clip> out;
    const Rational fps = s.settings.fps;
    Tick dur;
    if (a.info.isImage) {
        srcIn = 0;
        dur = srcOut > srcIn ? srcOut - srcIn : kDefaultStillDuration;
    } else {
        dur = srcOut - srcIn;
    }
    dur = floorToFrame(dur, fps);
    if (dur <= 0) dur = s.frameDur();
    const bool hasV = video && a.info.hasVideo;
    const bool hasA = audio && !a.info.audio.empty() && !a.info.isImage;
    Id link = (hasV && hasA) || (hasA && a.info.audio.size() > 1) ? s.newId() : 0;
    if (hasV) {
        auto tracks = ensureTracks(s, TrackKind::Video, vTrack + 1);
        Clip c = makeVideoClip(a, tracks[size_t(vTrack)], at, srcIn, dur);
        c.id = s.newId();
        c.linkId = link;
        out.push_back(std::move(c));
    }
    if (hasA) {
        for (size_t i = 0; i < a.info.audio.size(); ++i) {
            auto tracks = ensureTracks(s, TrackKind::Audio, aTrack + int(i) + 1);
            Clip c = makeAudioClip(a, int(i), tracks[size_t(aTrack) + i], at, srcIn, dur);
            c.id = s.newId();
            c.linkId = link;
            out.push_back(std::move(c));
        }
    }
    return out;
}

namespace {
bool onLockedTrack(const Sequence& s, const Clip& c) {
    const Track* t = s.track(c.trackId);
    return t && t->locked;
}
} // namespace

void linkClips(Sequence& s, const std::vector<Id>& ids) {
    std::vector<Clip*> cs;
    for (Id id : ids)
        if (Clip* c = s.clip(id); c && !onLockedTrack(s, *c)) cs.push_back(c);
    if (cs.size() < 2) return;
    const Id l = s.newId();
    for (Clip* c : cs) c->linkId = l;
}

void unlinkClips(Sequence& s, const std::vector<Id>& ids) {
    for (Id id : ids)
        if (Clip* c = s.clip(id); c && !onLockedTrack(s, *c)) c->linkId = 0;
}

void setEnabled(Sequence& s, const std::vector<Id>& ids, bool en) {
    for (Id id : ids)
        if (Clip* c = s.clip(id)) c->enabled = en;
}

void setSpeed(Sequence& s, const std::vector<Asset>& assets, Id clipId, double speed, bool reverse, bool ripple,
              bool frameBlend) {
    speed = std::clamp(speed, 0.1, 10.0);
    const Rational fps = s.settings.fps;
    const Tick fd = s.frameDur();
    const Clip* primary = s.clip(clipId);
    if (!primary) return;
    // Linked clips still in sync with the clicked one change together (so ripple keeps every track
    // aligned); partners trimmed apart (Alt) are re-timed on their own below.
    std::vector<Id> sync, loose;
    for (Id id : s.linkedClips(clipId)) {
        const Clip* c = s.clip(id);
        if (!c) continue;
        const Track* t = s.track(c->trackId);
        if (t && t->locked) continue;
        if (c->start == primary->start && c->duration == primary->duration) sync.push_back(id);
        else loose.push_back(id);
    }
    // longest duration a clip may have at the new speed without reading past its media
    auto maxDurFor = [&](const Clip& c) -> Tick {
        const Tick limit = c.kind == ClipKind::Media ? mediaLimit(assets, c) : 0;
        if (limit <= 0) return kPosInf;
        return std::max(fd, floorToFrame(Tick(double(std::max<Tick>(0, limit - c.sourceIn)) / speed), fps));
    };
    auto targetFor = [&](const Clip& c) {
        const Tick span = c.isHold() ? c.duration : c.sourceSpan();
        return std::min(maxDurFor(c), std::max(fd, floorToFrame(Tick(std::llround(double(span) / speed)), fps)));
    };
    // free time after a clip on its own track
    auto roomAfter = [&](const Clip& c) {
        Tick room = kPosInf;
        for (const auto& o : s.clips)
            if (o.trackId == c.trackId && o.id != c.id && o.start >= c.end()) room = std::min(room, o.start - c.end());
        return room;
    };
    auto retime = [&](Clip& c, Tick newDur) {
        const double oldSpeed = c.isHold() ? 1.0 : c.speed;
        c.speed = speed;
        c.reverse = reverse;
        if (c.kind == ClipKind::Media) c.frameBlend = frameBlend;
        c.duration = std::max(fd, newDur);
        // a minimum-length clip at the very end of its media: slide the source back so it fits
        if (const Tick limit = c.kind == ClipKind::Media ? mediaLimit(assets, c) : 0; limit > 0 && !c.isHold())
            if (c.sourceIn + c.sourceSpan() > limit) c.sourceIn = std::max<Tick>(0, limit - c.sourceSpan());
        // keep keyframes attached to content
        const double ratio = oldSpeed / speed;
        forEachParam(c, [&](Param& p) {
            for (auto& k : p.keys) k.t = floorToFrame(Tick(std::llround(double(k.t) * ratio)), fps);
        });
    };

    const Tick oldEnd = primary->end();
    const Tick oldDur = primary->duration;
    Tick delta = targetFor(*primary) - oldDur;
    for (Id id : sync) delta = std::min(delta, maxDurFor(*s.clip(id)) - oldDur);
    if (!ripple && delta > 0)
        for (Id id : sync) delta = std::min(delta, roomAfter(*s.clip(id)));  // grow into free space only
    const std::set<Id> gset(sync.begin(), sync.end());  // loose partners move with everything else
    if (ripple && delta > 0) shiftRight(s, oldEnd, delta, {}, gset);
    for (Id id : sync) retime(*s.clip(id), oldDur + delta);
    if (ripple && delta < 0) rippleShiftLeft(s, oldEnd, -delta);
    for (Id id : loose) {
        Clip& c = *s.clip(id);
        Tick target = targetFor(c);
        if (target > c.duration) target = std::min(target, c.duration + roomAfter(c));
        retime(c, target);
    }
}
Id addFrameHold(Sequence& s, Id clipId, Tick t) {
    Clip* c = s.clip(clipId);
    if (!c || !c->contains(t) || c->kind != ClipKind::Media) return 0;
    if (const Track* tr = s.track(c->trackId); tr && tr->locked) return 0;
    Tick src = c->sourceAt(t, s.frameDur());
    Id target = clipId;
    if (t > c->start) target = splitClip(s, clipId, t, 0);
    Clip* h = s.clip(target);
    if (!h) return 0;
    h->speed = 0;
    h->reverse = false;
    h->frameBlend = false;
    h->sourceIn = src;
    h->linkId = 0;
    return target;
}

Id insertFrameHoldSegment(Sequence& s, Id clipId, Tick t, Tick len) {
    Clip* c = s.clip(clipId);
    if (!c || !c->contains(t) || c->kind != ClipKind::Media || len <= 0) return 0;
    if (const Track* tr = s.track(c->trackId); tr && tr->locked) return 0;
    Tick src = c->sourceAt(t, s.frameDur());
    const Tick local = t - c->start;  // (c may dangle once the clip list grows)
    Clip hold = *c;
    if (t > c->start) splitClip(s, clipId, t, 0);
    insertSpace(s, t, len);
    hold.id = s.newId();
    hold.start = t;
    hold.duration = len;
    hold.speed = 0;
    hold.reverse = false;
    hold.frameBlend = false;
    hold.sourceIn = src;
    hold.linkId = 0;
    hold.fadeIn = hold.fadeOut = 0;
    forEachParam(hold, [&](Param& p) {
        if (!p.keys.empty()) {
            p.value = p.at(local);
            p.keys.clear();
        }
    });
    Id id = hold.id;
    s.clips.push_back(std::move(hold));
    return id;
}

Id addTransition(Sequence& s, Id trackId, Id clipA, Id clipB, TransitionType type, Tick duration,
                 TransitionAlign align) {
    const Clip* a = clipA ? s.clip(clipA) : nullptr;
    const Clip* b = clipB ? s.clip(clipB) : nullptr;
    if (!a && !b) return 0;
    if (a && b && (a->trackId != b->trackId || a->end() != b->start)) return 0;
    const Track* tr = s.track(a ? a->trackId : b->trackId);
    if (!tr || tr->locked) return 0;
    if (tr->kind == TrackKind::Audio) type = TransitionType::ConstantPower;
    else if (type == TransitionType::ConstantPower) type = TransitionType::CrossDissolve;
    if (!a || !b) align = a ? TransitionAlign::End : TransitionAlign::Start;
    s.transitions.erase(std::remove_if(s.transitions.begin(), s.transitions.end(),
                                       [&](const Transition& t) { return t.clipA == clipA && t.clipB == clipB; }),
                        s.transitions.end());
    Transition t;
    t.id = s.newId();
    t.trackId = trackId ? trackId : tr->id;
    t.clipA = clipA;
    t.clipB = clipB;
    t.type = type;
    t.duration = std::max(s.frameDur(), snapToFrame(duration, s.settings.fps));
    t.align = align;
    s.transitions.push_back(t);
    normalize(s);
    for (const auto& x : s.transitions)
        if (x.clipA == clipA && x.clipB == clipB) return x.id;
    return 0;
}

void removeTransition(Sequence& s, Id id) {
    s.transitions.erase(
        std::remove_if(s.transitions.begin(), s.transitions.end(), [&](const Transition& t) { return t.id == id; }),
        s.transitions.end());
}

bool transitionMissingHandles(const Sequence& s, const std::vector<Asset>& assets, const Transition& t) {
    if (!(t.clipA && t.clipB)) return false;
    const Clip* a = s.clip(t.clipA);
    const Clip* b = s.clip(t.clipB);
    if (!a || !b) return false;
    TickRange r = transitionRange(s, t);
    if (r.b > a->end() && !a->isHold()) {
        Tick lim = mediaLimit(assets, *a);
        Tick need = spanFor(*a, r.b - a->end());
        if (lim > 0 && (a->reverse ? a->sourceIn - need < 0 : a->sourceOut() + need > lim)) return true;
    }
    if (r.a < b->start && !b->isHold()) {
        Tick lim = mediaLimit(assets, *b);
        Tick need = spanFor(*b, b->start - r.a);
        if (lim > 0 && (b->reverse ? b->sourceOut() + need > lim : b->sourceIn - need < 0)) return true;
    }
    return false;
}

void sortCaptions(Sequence& s) {
    std::stable_sort(s.captions.begin(), s.captions.end(),
                     [](const Caption& a, const Caption& b) { return a.start < b.start; });
}

Id addCaption(Sequence& s, Tick start, Tick end, const std::string& text) {
    if (end <= start) return 0;
    Caption c;
    c.id = s.newId();
    c.start = start;
    c.end = end;
    c.text = text;
    s.captions.push_back(c);
    sortCaptions(s);
    return c.id;
}

bool splitCaption(Sequence& s, Id id, Tick t) {
    for (size_t i = 0; i < s.captions.size(); ++i) {
        if (s.captions[i].id != id) continue;
        Caption& c = s.captions[i];
        if (!(c.start < t && t < c.end)) return false;
        Caption r = c;
        r.id = s.newId();
        r.start = t;
        c.end = t;
        s.captions.push_back(r);
        sortCaptions(s);
        return true;
    }
    return false;
}

bool mergeCaptionWithNext(Sequence& s, Id id) {
    sortCaptions(s);
    for (size_t i = 0; i + 1 < s.captions.size(); ++i) {
        if (s.captions[i].id != id) continue;
        Caption& a = s.captions[i];
        const Caption& b = s.captions[i + 1];
        a.end = std::max(a.end, b.end);
        a.text = a.text + (a.text.empty() || b.text.empty() ? "" : " ") + b.text;
        s.captions.erase(s.captions.begin() + long(i) + 1);
        return true;
    }
    return false;
}

void deleteCaptions(Sequence& s, const std::vector<Id>& ids) {
    std::set<Id> del(ids.begin(), ids.end());
    s.captions.erase(std::remove_if(s.captions.begin(), s.captions.end(),
                                    [&](const Caption& c) { return del.count(c.id) > 0; }),
                     s.captions.end());
}

Id addMarker(Sequence& s, Tick t, const std::string& name) {
    Marker m;
    m.id = s.newId();
    m.t = t;
    m.name = name;
    s.markers.push_back(m);
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return m.id;
}

std::vector<Tick> editPoints(const Sequence& s) {
    std::vector<Tick> pts{0};
    for (const auto& c : s.clips) {
        pts.push_back(c.start);
        pts.push_back(c.end());
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    return pts;
}

// --- sequences ------------------------------------------------------------------------------------
namespace {
std::vector<Sequence> takeAllSequences(Project& p) {
    std::vector<Sequence> all = std::move(p.otherSequences);
    all.insert(all.begin() + std::clamp(p.activeSequence, 0, int(all.size())), std::move(p.sequence));
    p.otherSequences.clear();
    return all;
}
void putAllSequences(Project& p, std::vector<Sequence> all, int active) {
    active = std::clamp(active, 0, int(all.size()) - 1);
    p.sequence = std::move(all[size_t(active)]);
    all.erase(all.begin() + active);
    p.otherSequences = std::move(all);
    p.activeSequence = active;
}
} // namespace

int sequenceCount(const Project& p) { return int(p.otherSequences.size()) + 1; }

void switchSequence(Project& p, int tab) {
    if (tab < 0 || tab >= sequenceCount(p) || tab == p.activeSequence) return;
    putAllSequences(p, takeAllSequences(p), tab);
}

void insertSequence(Project& p, int at, Sequence s) {
    if (s.uid == 0 || p.sequenceByUid(s.uid)) s.uid = p.nextSequenceUid++;  // copies get their own identity
    auto all = takeAllSequences(p);
    at = std::clamp(at, 0, int(all.size()));
    all.insert(all.begin() + at, std::move(s));
    putAllSequences(p, std::move(all), at);
}

bool removeSequence(Project& p, int tab) {
    if (sequenceCount(p) < 2 || tab < 0 || tab >= sequenceCount(p)) return false;
    int active = p.activeSequence;
    auto all = takeAllSequences(p);
    all.erase(all.begin() + tab);
    if (active > tab || (active == tab && active > 0)) --active;  // the open one went: its left neighbour opens
    putAllSequences(p, std::move(all), std::max(0, active));
    return true;
}

void renameSequence(Project& p, int tab, const std::string& name) {
    if (name.empty()) return;
    if (tab == p.activeSequence) p.sequence.name = name;
    else if (tab >= 0 && tab < sequenceCount(p))
        p.otherSequences[size_t(tab < p.activeSequence ? tab : tab - 1)].name = name;
}

// --- nesting -----------------------------------------------------------------------------------------
std::vector<Id> nestClips(Project& p, const std::vector<Id>& ids, const std::string& name) {
    Sequence& s = p.sequence;
    std::vector<Clip> picked;
    std::set<Id> seen;
    for (Id id : ids)
        if (const Clip* c = s.clip(id); c && seen.insert(id).second) {
            const Track* t = s.track(c->trackId);
            if (t && !t->locked && t->kind != TrackKind::Caption) picked.push_back(*c);
        }
    if (picked.empty()) return {};
    Tick a = kPosInf, b = kNegInf;
    int vTrack = INT_MAX, aTrack = INT_MAX;
    for (const auto& c : picked) {
        a = std::min(a, c.start);
        b = std::max(b, c.end());
        const Track* t = s.track(c.trackId);
        int& lowest = t->kind == TrackKind::Video ? vTrack : aTrack;
        lowest = std::min(lowest, s.trackIndexInKind(c.trackId));
    }
    // the new sequence: same format, same track layout, clips moved to start at 0
    Sequence ns = makeDefaultSequence(s.settings);
    ns.uid = p.nextSequenceUid++;
    ns.name = name;
    std::map<Id, Id> idMap, linkMap;
    for (const auto& c : picked) {
        const Track* t = s.track(c.trackId);
        const int idx = s.trackIndexInKind(c.trackId);
        const Id nt = ensureTracks(ns, t->kind, idx + 1)[size_t(idx)];
        Clip x = c;
        x.id = ns.newId();
        x.trackId = nt;
        x.start -= a;
        if (x.linkId) {
            auto it = linkMap.find(x.linkId);
            if (it == linkMap.end()) it = linkMap.emplace(x.linkId, ns.newId()).first;
            x.linkId = it->second;
        }
        idMap[c.id] = x.id;
        ns.clips.push_back(std::move(x));
    }
    for (const auto& tr : s.transitions) {
        const bool inA = !tr.clipA || idMap.count(tr.clipA), inB = !tr.clipB || idMap.count(tr.clipB);
        if (!inA || !inB || (!tr.clipA && !tr.clipB)) continue;
        Transition x = tr;
        x.id = ns.newId();
        x.clipA = tr.clipA ? idMap[tr.clipA] : 0;
        x.clipB = tr.clipB ? idMap[tr.clipB] : 0;
        const Clip* ref = ns.clip(x.clipA ? x.clipA : x.clipB);
        x.trackId = ref->trackId;
        ns.transitions.push_back(x);
    }
    normalize(ns);
    std::vector<Id> gone;
    for (const auto& c : picked) gone.push_back(c.id);
    deleteClips(s, gone);
    // the nest replaces them: one video clip (and one audio clip carrying the nested mix), linked
    std::vector<Clip> nests;
    auto make = [&](TrackKind k, int idx) {
        Clip n;
        n.kind = ClipKind::Nested;
        n.nestedSeq = ns.uid;
        n.name = name;
        n.trackId = ensureTracks(s, k, idx + 1)[size_t(idx)];
        n.start = a;
        n.duration = b - a;
        initBuiltinEffects(n, k);
        nests.push_back(n);
    };
    if (vTrack != INT_MAX) make(TrackKind::Video, vTrack);
    if (aTrack != INT_MAX) make(TrackKind::Audio, aTrack);
    if (nests.size() == 2) nests[0].linkId = nests[1].linkId = s.newId();
    std::vector<Id> out = overwriteClips(s, nests);
    // the nested sequence goes right after the open one, which stays open
    p.otherSequences.insert(p.otherSequences.begin() + std::clamp(p.activeSequence, 0, int(p.otherSequences.size())), std::move(ns));
    return out;
}

// --- animation presets -------------------------------------------------------------------------------
bool applyAnimation(Clip& c, Animation a, Tick fd, int W, int H) {
    if (c.motion.type.empty() || c.opacity.type.empty() || c.duration < 3 * fd) return false;
    auto snap = [&](Tick t) { return std::clamp<Tick>(t / fd * fd, 0, c.duration - fd); };
    const Tick d = snap(std::min<Tick>(kTicksPerSecond * 2 / 5, c.duration / 3));  // ~0.4 s
    const Tick end = snap(c.duration - fd);
    auto base = [](const Param& p, Tick t) { return p.animated() ? p.at(t) : p.value; };
    auto key = [](Param& p, Tick t, double v, Interp in = Interp::Linear) { p.addKey(t, v, in); };
    Param& op = c.opacity.params["opacity"];
    Param& px = c.motion.params["posX"];
    Param& py = c.motion.params["posY"];
    Param& sc = c.motion.params["scale"];
    auto fadeIn = [&] {
        const double b = base(op, d);
        key(op, 0, 0, Interp::EaseOut);
        key(op, d, b);
    };
    switch (a) {
    case Animation::FadeIn: fadeIn(); break;
    case Animation::FadeOut: {
        const double b = base(op, end - d);
        key(op, end - d, b, Interp::EaseIn);
        key(op, end, 0);
        break;
    }
    case Animation::SlideUpIn: {
        const double b = base(py, d);
        key(py, 0, b + 0.12 * H, Interp::EaseOut);
        key(py, d, b);
        fadeIn();
        break;
    }
    case Animation::SlideLeftIn:
    case Animation::SlideRightIn: {
        const double b = base(px, d);
        key(px, 0, b + (a == Animation::SlideLeftIn ? -W : W), Interp::EaseOut);
        key(px, d, b);
        break;
    }
    case Animation::PopIn: {
        const double b = base(sc, d);
        key(sc, 0, 0, Interp::EaseOut);
        key(sc, snap(d * 7 / 10), b * 1.08, Interp::Ease);
        key(sc, d, b);
        break;
    }
    case Animation::SlowZoom: {
        const double b = base(sc, 0);
        key(sc, 0, b);
        key(sc, end, b * 1.15);
        break;
    }
    case Animation::Typewriter: {
        if (c.kind != ClipKind::Text) return false;
        Effect* tw = nullptr;
        for (auto& e : c.effects)
            if (e.type == fx::Typewriter) tw = &e;
        if (!tw) {
            c.effects.push_back(makeEffect(fx::Typewriter));
            tw = &c.effects.back();
        }
        Param& pr = tw->params["progress"];
        pr.keys.clear();
        // about 30 characters a second, at most half the clip
        const Tick len = snap(std::min<Tick>(c.duration / 2, std::max<Tick>(fd * 3, Tick(c.text.size()) * kTicksPerSecond / 30)));
        key(pr, 0, 0);
        key(pr, std::max(fd, len), 100);
        break;
    }
    case Animation::SlideDownOut: {
        const double b = base(py, end - d);
        key(py, end - d, b, Interp::EaseIn);
        key(py, end, b + 0.12 * H);
        const double o = base(op, end - d);
        key(op, end - d, o, Interp::EaseIn);
        key(op, end, 0);
        break;
    }
    }
    return true;
}

bool punchIn(Clip& c, Tick local, double px, double py, double factor, Tick ramp, Tick hold, Tick fd, int W, int H) {
    if (c.motion.type.empty() || factor <= 0) return false;
    local = local / fd * fd;
    const Tick last = c.duration - fd;
    if (local < 0 || local + 2 * fd > last) return false;
    // fit ramp - hold - ramp into what is left of the clip
    Tick r = std::max(fd, ramp / fd * fd), h = std::max<Tick>(0, hold / fd * fd);
    if (local + 2 * r + h > last) {
        const Tick room = last - local;
        r = std::max(fd, room / 4 / fd * fd);
        h = std::max<Tick>(0, (room - 2 * r) / fd * fd);
    }
    Param& sx = c.motion.params["scale"];
    Param& sw = c.motion.params["scaleW"];
    Param& x = c.motion.params["posX"];
    Param& y = c.motion.params["posY"];
    auto at = [&](const Param& p) { return p.animated() ? p.at(local) : p.value; };
    const double s0 = at(sx), w0 = at(sw), x0 = at(x), y0 = at(y);
    // scaling pivots on the position (frame centre + pos): move it so (px, py) stays put
    const double x1 = x0 + (1 - factor) * (px - (W / 2.0 + x0)), y1 = y0 + (1 - factor) * (py - (H / 2.0 + y0));
    const Tick t1 = local + r, t2 = t1 + h, t3 = t2 + r;
    auto keys = [&](Param& p, double a, double b) {
        p.addKey(local, a, Interp::Ease);
        p.addKey(t1, b, Interp::Ease);
        p.addKey(t2, b, Interp::Ease);
        p.addKey(t3, a, Interp::Ease);
    };
    keys(sx, s0, s0 * factor);
    if (c.motion.p("uniform", 0) < 0.5) keys(sw, w0, w0 * factor);
    keys(x, x0, x1);
    keys(y, y0, y1);
    return true;
}

// --- silence ----------------------------------------------------------------------------------------
std::vector<TickRange> findSilences(const std::vector<float>& levels, Tick origin, Tick window, double thresholdDb,
                                    Tick minLength, Tick pad, Tick fd) {
    std::vector<TickRange> out;
    const float thr = float(std::pow(10.0, thresholdDb / 20.0));
    size_t i = 0;
    while (i < levels.size()) {
        if (levels[i] >= thr) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < levels.size() && levels[j] < thr) ++j;
        Tick a = origin + Tick(i) * window + pad, b = origin + Tick(j) * window - pad;
        a = (a + fd - 1) / fd * fd;  // inside the pause: round the start up, the end down
        b = b / fd * fd;
        if (b - a >= std::max(minLength - 2 * pad, fd)) out.push_back({a, b});
        i = j;
    }
    return out;
}

void removeSpans(Sequence& s, std::vector<TickRange> spans) {
    std::sort(spans.begin(), spans.end(), [](const TickRange& x, const TickRange& y) { return x.a > y.a; });
    for (const auto& r : spans) {
        if (r.b <= r.a) continue;
        for (const auto& t : s.tracks) {
            if (t.locked) continue;
            if (t.kind == TrackKind::Caption) {
                std::vector<Id> del;
                for (auto& c : s.captions) {
                    if (c.start >= r.a && c.end <= r.b) del.push_back(c.id);
                    else if (c.start < r.a && c.end > r.a) c.end -= std::min(c.end, r.b) - r.a;  // loses the removed part
                    else if (c.start < r.b && c.end > r.b) c.start = r.b;                       // (then shifted left)
                }
                deleteCaptions(s, del);
                continue;
            }
            clearRange(s, t.id, r.a, r.b);
        }
        rippleShiftLeft(s, r.a, r.b - r.a);
    }
}

// --- speed ramp ---------------------------------------------------------------------------------------
bool speedRamp(Sequence& s, const std::vector<Asset>& assets, Id clipId, Tick t, double slow, Tick slowSource,
               Tick rampSource, int steps) {
    const Clip* c = s.clip(clipId);
    const Tick fd = s.frameDur();
    if (!c || c->kind != ClipKind::Media || c->speed != 1.0 || c->reverse || c->isHold()) return false;
    if (const Track* tr = s.track(c->trackId); !tr || tr->locked) return false;
    slow = std::clamp(slow, 0.1, 1.0);
    steps = std::clamp(steps, 1, 12);
    // segment boundaries in timeline time (speed 1: timeline offsets = media offsets)
    auto f = [&](Tick x) { return x / fd * fd; };
    const Tick seg = std::max(fd, f(rampSource / steps));
    const Tick half = std::max(fd, f(slowSource / 2));
    const Tick a = f(t) - half - seg * steps, b = f(t) + half + seg * steps;
    if (a <= c->start || b >= c->end()) return false;
    std::vector<Tick> cuts;
    for (int i = 0; i <= steps; ++i) cuts.push_back(a + seg * i);           // ramp in
    for (int i = 0; i <= steps; ++i) cuts.push_back(f(t) + half + seg * i);  // ramp out
    std::vector<double> speeds;  // one per segment between consecutive cuts
    auto ease = [](double u) { return u * u * (3 - 2 * u); };
    for (int i = 0; i < steps; ++i) speeds.push_back(1.0 + (slow - 1.0) * ease((i + 0.5) / steps));
    speeds.push_back(slow);  // the slow middle
    for (int i = 0; i < steps; ++i) speeds.push_back(slow + (1.0 - slow) * ease((i + 0.5) / steps));
    // split every cut (left to right; razor keeps linked audio in step), collecting the pieces
    std::vector<Id> pieces;
    Id cur = clipId;
    for (Tick x : cuts) {
        std::vector<Id> right = razor(s, cur, x);
        if (right.empty()) return false;
        pieces.push_back(right.front());  // the piece starting at x
        cur = right.front();
    }
    pieces.pop_back();  // the last piece (after the ramp out) keeps 100 %
    // speeds right to left so earlier pieces stay where they are while later ones ripple
    for (int i = int(pieces.size()) - 1; i >= 0; --i) setSpeed(s, assets, pieces[size_t(i)], speeds[size_t(i)], false, true, true);
    return true;
}

// --- beats ------------------------------------------------------------------------------------------
std::vector<Tick> findBeats(const std::vector<float>& levels, Tick origin, Tick window, Tick minGap, Tick fd,
                            double sensitivity) {
    // onset strength: positive change of the (log-ish) level; a beat is a local peak of it that
    // stands above the average strength of the surrounding second
    const size_t n = levels.size();
    std::vector<double> flux(n, 0.0);
    for (size_t i = 1; i < n; ++i) {
        const double a = std::log1p(100.0 * levels[i - 1]), b = std::log1p(100.0 * levels[i]);
        flux[i] = std::max(0.0, b - a);
    }
    const int half = std::max<int>(1, int(kTicksPerSecond / 2 / std::max<Tick>(1, window)));
    std::vector<Tick> out;
    Tick last = kNegInf;
    double sum = 0;
    size_t lo = 0, hi = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        // running mean of flux over [i - half, i + half]
        while (hi < n && hi <= i + size_t(half)) sum += flux[hi++];
        while (lo + size_t(half) < i) sum -= flux[lo++];
        const double mean = sum / double(hi - lo);
        const bool peak = flux[i] >= flux[i - 1] && flux[i] > flux[i + 1];
        if (!peak || flux[i] < mean * (1.0 + 1.5 / std::max(0.1, sensitivity)) || flux[i] < 0.05) continue;
        const Tick t = (origin + Tick(i) * window) / fd * fd;
        if (t - last < minGap) continue;
        out.push_back(t);
        last = t;
    }
    return out;
}

void changeFrameRate(Sequence& s, const std::vector<Asset>& assets, Rational fps) {
    if (!fps.valid()) return;
    s.settings.fps = fps;
    const Tick fd = frameDuration(fps);
    auto snap = [&](Tick t) { return snapToFrame(t, fps); };
    for (const auto& tr : s.tracks) {
        std::vector<Id> ids;
        for (const Clip* c : s.clipsOnTrack(tr.id)) ids.push_back(c->id);  // in time order
        Tick prevEnd = 0;
        for (Id id : ids) {
            Clip& c = *s.clip(id);
            Tick a = std::max(snap(c.start), prevEnd);
            Tick b = std::max(snap(c.end()), a + fd);
            // a moved head keeps showing the same moment at the same time
            const Tick dh = a - c.start;
            if (dh != 0 && !c.isHold() && !c.reverse)
                c.sourceIn = std::max<Tick>(0, c.sourceIn + (c.speed == 1.0 ? dh : Tick(std::llround(double(dh) * c.speed))));
            shiftKeys(c, dh);
            c.start = a;
            c.duration = b - a;
            const Tick limit = mediaLimit(assets, c);
            while (limit > 0 && !c.isHold() && c.duration > fd && c.sourceOut() > limit) c.duration -= fd;
            if (limit > 0 && !c.isHold() && c.sourceOut() > limit) c.sourceIn = std::max<Tick>(0, limit - c.sourceSpan());  // one frame left
            prevEnd = c.end();
        }
    }
    for (auto& t : s.transitions) t.duration = std::max(fd, snap(t.duration));
    for (auto& m : s.markers) m.t = snap(m.t);
    if (s.inPoint != kInvalidTick) s.inPoint = snap(s.inPoint);
    if (s.outPoint != kInvalidTick) s.outPoint = snap(s.outPoint);
    std::sort(s.captions.begin(), s.captions.end(), [](const Caption& x, const Caption& y) { return x.start < y.start; });
    Tick capEnd = 0;
    for (auto& cap : s.captions) {
        cap.start = std::max(snap(cap.start), capEnd);
        cap.end = std::max(snap(cap.end), cap.start + fd);
        capEnd = cap.end;
    }
    normalize(s);
}

int replaceMedia(Sequence& s, const Asset& a, const std::vector<Id>& clipIds, Tick in) {
    const Tick fd = s.frameDur();
    std::set<Id> done;
    int changed = 0;
    for (Id id : clipIds) {
        const Clip* primary = s.clip(id);
        if (!primary || done.count(id)) continue;
        const Tick base = primary->sourceIn;
        for (Id pid : s.linkedClips(id)) {
            if (!done.insert(pid).second) continue;
            Clip* c = s.clip(pid);
            const Track* tr = c ? s.track(c->trackId) : nullptr;
            if (!c || !tr || tr->locked || c->kind != ClipKind::Media || c->reverse) continue;
            if (tr->kind == TrackKind::Video && !a.info.hasVideo && !a.info.isImage) continue;
            if (tr->kind == TrackKind::Audio) {
                if (a.info.audio.empty()) continue;
                if (c->stream >= int(a.info.audio.size())) c->stream = 0;
            }
            const Tick newIn = std::max<Tick>(0, in + (c->sourceIn - base));
            if (!a.info.isImage && newIn >= a.info.duration) continue;
            Clip k = *c;
            k.sourceIn = newIn;
            if (!a.info.isImage && !k.isHold() && k.sourceOut() > a.info.duration) {
                // as many whole frames as the new media still covers at this speed
                k.duration = Tick(double(a.info.duration - newIn) / k.speed) / fd * fd;
                while (k.duration > 0 && k.sourceOut() > a.info.duration) k.duration -= fd;
                if (k.duration < fd) continue;
            }
            k.assetId = a.id;
            k.name = a.name;
            *c = k;
            ++changed;
        }
    }
    return changed;
}

int fitAudioToEnd(Sequence& s, const std::vector<Asset>& assets, Id clipId, Tick end, Tick fade) {
    const Tick fd = s.frameDur();
    const Clip* c0 = s.clip(clipId);
    const Track* tr = c0 ? s.track(c0->trackId) : nullptr;
    if (!c0 || !tr || tr->locked || tr->kind != TrackKind::Audio || c0->kind != ClipKind::Media || c0->speed != 1.0 ||
        c0->reverse)
        return 0;
    const Id track = c0->trackId;
    end = end / fd * fd;
    for (const Clip* o : s.clipsOnTrack(track))  // stop at the next clip on the track
        if (o->id != clipId && o->start >= c0->end() && o->start < end) end = o->start;
    if (end < c0->start + 2 * fd) return 0;
    auto fadeOut = [&](Clip& c) { c.fadeOut = std::min(fade, c.duration / 2) / fd * fd; };
    if (c0->end() >= end) {
        Clip& c = *s.clip(clipId);
        c.duration = end - c.start;
        fadeOut(c);
        return 1;
    }
    const Tick media = mediaLimit(assets, *c0);
    if (media <= 0) return 0;
    const Tick in = c0->sourceIn;
    const Tick xf = std::max(fd, std::min(kTicksPerSecond, (media - in) / 4) / fd * fd);  // crossfade
    const Tick loopLen = (media - in - xf) / fd * fd;  // each repeat keeps xf of tail for the fade
    if (loopLen < 2 * fd) return 0;
    {
        Clip& c = *s.clip(clipId);
        if (c.sourceOut() + xf > media) c.duration = std::max(fd, (media - xf - c.sourceIn) / fd * fd);
        c.fadeOut = 0;
    }
    Clip proto = *s.clip(clipId);
    proto.id = 0;
    proto.linkId = 0;
    proto.fadeIn = proto.fadeOut = 0;
    proto.sourceIn = in;
    Id prev = clipId;
    Tick pos = s.clip(clipId)->end();
    int pieces = 1;
    while (pos < end && pieces < 1000) {
        Clip k = proto;
        k.start = pos;
        k.duration = std::min(loopLen, end - pos);
        if (k.duration < fd) break;
        const Id id = overwriteClips(s, {k}).front();
        addTransition(s, track, prev, id, TransitionType::ConstantPower, xf, TransitionAlign::Start);
        prev = id;
        pos += k.duration;
        ++pieces;
    }
    fadeOut(*s.clip(prev));
    return pieces;
}

TickRange instantReplay(Sequence& s, Tick t, Tick len, double speed) {
    const Tick fd = s.frameDur();
    t = t / fd * fd;
    const Tick a = std::max<Tick>(0, (t - len) / fd * fd);
    if (a >= t || speed <= 0) return {};
    std::vector<Clip> copies;
    for (const auto& c : s.clips) {
        const Track* tr = s.track(c.trackId);
        if (!tr || tr->locked || tr->kind == TrackKind::Caption || c.reverse) continue;
        const Tick ca = std::max(c.start, a), cb = std::min(c.end(), t);
        if (cb - ca < fd) continue;
        Clip k = c;
        k.id = 0;  // a new clip (overwriteClips assigns the id)
        // trim to the window, then stretch to the replay speed
        const Tick head = ca - c.start;
        if (!c.isHold()) k.sourceIn = c.sourceIn + (c.speed == 1.0 ? head : Tick(std::llround(double(head) * c.speed)));
        shiftKeys(k, head);
        k.start = t + Tick(std::llround(double(ca - a) / speed)) / fd * fd;
        k.duration = std::max(fd, Tick(double(cb - ca) / speed) / fd * fd);
        if (!c.isHold()) k.speed = c.speed * speed;
        forEachParam(k, [&](Param& p) {
            for (auto& key : p.keys) key.t = Tick(std::llround(double(key.t) / speed));
        });
        k.fadeIn = k.fadeOut = 0;
        k.frameBlend = k.frameBlend || speed < 1.0;
        copies.push_back(std::move(k));
    }
    if (copies.empty()) return {};
    const Tick total = std::max(fd, Tick(std::llround(double(t - a) / speed)) / fd * fd);
    insertSpace(s, t, total);
    // linked groups stay linked among the copies
    std::map<Id, Id> links;
    for (auto& k : copies) {
        if (!k.linkId) continue;
        auto [it, fresh] = links.emplace(k.linkId, 0);
        if (fresh) it->second = s.newId();
        k.linkId = it->second;
    }
    overwriteClips(s, std::move(copies));
    return {t, t + total};
}

std::vector<Tick> findHighlights(const std::vector<float>& levels, Tick origin, Tick window, int maxCount, Tick minGap,
                                 Tick fd) {
    const size_t n = levels.size();
    if (n == 0 || window <= 0 || maxCount <= 0) return {};
    const auto per = [&](double sec) { return std::max<size_t>(1, size_t(sec * double(kTicksPerSecond) / double(window))); };
    // short-term level (dB of the mean power over 0.3 s, centred)
    const size_t sw = per(0.3);
    std::vector<double> pre(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + double(levels[i]) * double(levels[i]);
    std::vector<double> L(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i >= sw / 2 ? i - sw / 2 : 0, b = std::min(n, a + sw);
        L[i] = 10.0 * std::log10((pre[b] - pre[a]) / double(b - a) + 1e-9);
    }
    // baseline: median of 1 s block levels within +-15 s
    const size_t bw = per(1.0);
    const size_t nb = (n + bw - 1) / bw;
    std::vector<double> block(nb);
    for (size_t k = 0; k < nb; ++k) {
        const size_t a = k * bw, b = std::min(n, a + bw);
        block[k] = 10.0 * std::log10((pre[b] - pre[a]) / double(b - a) + 1e-9);
    }
    std::vector<double> base(nb);
    for (size_t k = 0; k < nb; ++k) {
        const size_t a = k >= 15 ? k - 15 : 0, b = std::min(nb, k + 16);
        std::vector<double> w(block.begin() + std::ptrdiff_t(a), block.begin() + std::ptrdiff_t(b));
        std::nth_element(w.begin(), w.begin() + std::ptrdiff_t(w.size() / 2), w.end());
        base[k] = w[w.size() / 2];
    }
    std::vector<double> score(n);
    for (size_t i = 0; i < n; ++i) score[i] = L[i] > -45.0 ? L[i] - base[i / bw] : 0.0;
    // candidates: local maxima of the score that stand 8 dB above the surroundings
    std::vector<size_t> cand;
    for (size_t i = 1; i + 1 < n; ++i)
        if (score[i] >= 8.0 && score[i] >= score[i - 1] && score[i] > score[i + 1]) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [&](size_t a, size_t b) { return score[a] > score[b]; });
    std::vector<Tick> out;
    std::vector<size_t> taken;
    for (size_t i : cand) {
        bool tooClose = false;
        for (size_t j : taken) tooClose |= Tick(i > j ? i - j : j - i) * window < minGap;
        if (tooClose) continue;
        taken.push_back(i);
        // back to where it started getting loud
        size_t on = i;
        while (on > 0 && score[on - 1] > score[i] / 2) --on;
        out.push_back((origin + Tick(on) * window) / fd * fd);
        if (int(out.size()) >= maxCount) break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace cut::edit
