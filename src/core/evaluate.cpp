#include "core/evaluate.h"

#include <cmath>

namespace cut {

Tick sourceAtExtended(const Clip& c, Tick t, Tick frameDur) {
    if (c.isHold()) return c.sourceIn;
    if (t >= c.start && t < c.end()) return c.sourceAt(t, frameDur);
    Tick local = t - c.start;
    auto scale = [&](Tick x) { return c.speed == 1.0 ? x : Tick(std::llround(double(x) * c.speed)); };
    if (c.reverse) return c.sourceIn + scale(c.duration - frameDur - local);
    return c.sourceIn + scale(local);
}

std::vector<VideoLayer> evaluateVideo(const Sequence& s, Tick t) {
    std::vector<VideoLayer> out;
    const Tick fd = s.frameDur();
    int vi = 0;
    for (const auto& tr : s.tracks) {
        if (tr.kind != TrackKind::Video) continue;
        int idx = vi++;
        if (!tr.visible) continue;
        // transitions first
        const Transition* active = nullptr;
        TickRange range;
        for (const auto& x : s.transitions) {
            if (x.trackId != tr.id) continue;
            TickRange r = transitionRange(s, x);
            if (r.contains(t)) {
                active = &x;
                range = r;
                break;
            }
        }
        if (active) {
            VideoLayer L;
            L.trackIndex = idx;
            L.transition = active;
            L.progress = range.len() > 0 ? double(t - range.a) / double(range.len()) : 1.0;
            const Clip* a = active->clipA ? s.clip(active->clipA) : nullptr;
            const Clip* b = active->clipB ? s.clip(active->clipB) : nullptr;
            if (a && a->enabled) L.a = {a, t - a->start, sourceAtExtended(*a, t, fd)};
            if (b && b->enabled) L.b = {b, t - b->start, sourceAtExtended(*b, t, fd)};
            if (L.a.clip || L.b.clip) out.push_back(L);
            continue;
        }
        for (const auto& c : s.clips) {
            if (c.trackId != tr.id || !c.contains(t)) continue;
            if (!c.enabled) break;
            VideoLayer L;
            L.trackIndex = idx;
            L.a = {&c, t - c.start, c.sourceAt(t, fd)};
            out.push_back(L);
            break;
        }
    }
    return out;
}

const Caption* captionAt(const Sequence& s, Tick t) {
    for (const auto& tr : s.tracks)
        if (tr.kind == TrackKind::Caption && !tr.visible) return nullptr;
    for (const auto& c : s.captions)
        if (t >= c.start && t < c.end) return &c;
    return nullptr;
}

bool frameIsEmpty(const Sequence& s, Tick t) { return evaluateVideo(s, t).empty() && !captionAt(s, t); }

std::vector<const Clip*> clipsInRange(const Sequence& s, Tick t0, Tick t1, TrackKind kind) {
    std::vector<const Clip*> r;
    for (const auto& c : s.clips) {
        if (!c.enabled || c.start >= t1 || c.end() <= t0) continue;
        const Track* tr = s.track(c.trackId);
        if (!tr || tr->kind != kind) continue;
        if (kind == TrackKind::Video && !tr->visible) continue;
        r.push_back(&c);
    }
    return r;
}

} // namespace cut
