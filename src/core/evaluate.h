#pragma once
// Timeline evaluation: which clips contribute to the picture at time t, and from which source
// time. Shared by preview and export so both render exactly the same thing.
#include "core/model.h"

#include <vector>

namespace cut {

struct LayerClip {
    const Clip* clip = nullptr;
    Tick local = 0;   // clip-local time for animated parameters (may be outside [0,dur) in handles)
    Tick source = 0;  // source time to show (unclamped; providers clamp to the media)
};

struct VideoLayer {
    int trackIndex = 0;  // V1 = 0
    LayerClip a;         // the clip, or the outgoing clip of a transition (may be empty)
    LayerClip b;         // incoming clip of a transition (may be empty)
    const Transition* transition = nullptr;
    double progress = 0;  // 0..1 through the transition
};

// Source time for timeline time t, extrapolating outside the clip (transition handles).
Tick sourceAtExtended(const Clip& c, Tick t, Tick frameDur);
// Layers bottom (V1) to top. Skips hidden tracks and disabled clips.
std::vector<VideoLayer> evaluateVideo(const Sequence& s, Tick t);
const Caption* captionAt(const Sequence& s, Tick t);
// True if any visible video clip / caption needs drawing at t.
bool frameIsEmpty(const Sequence& s, Tick t);

// Clips whose media is needed in [t0, t1) (for prefetch).
std::vector<const Clip*> clipsInRange(const Sequence& s, Tick t0, Tick t1, TrackKind kind);

} // namespace cut
