#include "core/model.h"

#include <algorithm>
#include <cmath>

namespace cut {

// ---------------------------------------------------------------------------------------------
// Param
double Param::at(Tick local) const {
    if (keys.empty()) return value;
    if (local <= keys.front().t) return keys.front().v;
    if (local >= keys.back().t) return keys.back().v;
    auto it = std::upper_bound(keys.begin(), keys.end(), local,
                               [](Tick t, const Keyframe& k) { return t < k.t; });
    const Keyframe& b = *it;
    const Keyframe& a = *(it - 1);
    if (a.interp == Interp::Hold) return a.v;
    double u = double(local - a.t) / double(b.t - a.t);
    if (a.interp == Interp::Ease) u = u * u * (3.0 - 2.0 * u);
    else if (a.interp == Interp::EaseIn) u = u * u;                    // starts slow
    else if (a.interp == Interp::EaseOut) u = 1.0 - (1.0 - u) * (1.0 - u);  // lands softly
    return a.v + (b.v - a.v) * u;
}

const Keyframe* Param::keyAt(Tick local, Tick tol) const {
    for (const auto& k : keys)
        if (std::llabs(k.t - local) <= tol) return &k;
    return nullptr;
}

void Param::addKey(Tick local, double v, Interp interp) {
    for (auto& k : keys)
        if (k.t == local) {
            k.v = v;
            return;
        }
    keys.push_back({local, v, interp});
    std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

void Param::set(Tick local, double v) {
    if (keys.empty()) value = v;
    else addKey(local, v, Interp::Linear);
}

bool Param::removeKeyAt(Tick local, Tick tol) {
    for (auto it = keys.begin(); it != keys.end(); ++it)
        if (std::llabs(it->t - local) <= tol) {
            if (keys.size() == 1) value = it->v;  // keep the last value when un-animating
            keys.erase(it);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Effect registry
namespace {
ParamDef num(const char* id, const char* label, double def, double mn, double mx, double step,
             int decimals, const char* unit = "", bool anim = true) {
    ParamDef p;
    p.id = id;
    p.label = label;
    p.def = def;
    p.min = mn;
    p.max = mx;
    p.step = step;
    p.decimals = decimals;
    p.unit = unit;
    p.animatable = anim;
    return p;
}
ParamDef boolean(const char* id, const char* label, bool def) {
    ParamDef p = num(id, label, def ? 1 : 0, 0, 1, 1, 0, "", false);
    p.isBool = true;
    return p;
}
ParamDef enumeration(const char* id, const char* label, std::vector<std::string> labels) {
    ParamDef p = num(id, label, 0, 0, double(labels.size() - 1), 1, 0, "", false);
    p.enumLabels = std::move(labels);
    return p;
}

std::vector<EffectDef> buildRegistry() {
    std::vector<EffectDef> r;
    {
        EffectDef d{fx::Motion, "Motion", "Built-in", true, true, {}};
        d.params = {
            num("posX", "Position X", 0, -20000, 20000, 1, 1, "px"),
            num("posY", "Position Y", 0, -20000, 20000, 1, 1, "px"),
            num("scale", "Scale", 100, 0, 1000, 0.5, 1, "%"),
            num("scaleW", "Scale Width", 100, 0, 1000, 0.5, 1, "%"),
            boolean("uniform", "Uniform Scale", true),
            num("rotation", "Rotation", 0, -3600, 3600, 0.5, 1, "\xC2\xB0"),
            num("anchorX", "Anchor X", 0, -20000, 20000, 1, 1, "px"),
            num("anchorY", "Anchor Y", 0, -20000, 20000, 1, 1, "px"),
        };
        r.push_back(d);
    }
    {
        EffectDef d{fx::Opacity, "Opacity", "Built-in", true, true, {}};
        d.params = {num("opacity", "Opacity", 100, 0, 100, 1, 1, "%"),
                    enumeration("blend", "Blend Mode", {"Normal", "Add", "Multiply", "Screen"})};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Volume, "Volume", "Built-in", true, false, {}};
        d.params = {num("level", "Level", 0, -96, 12, 0.1, 1, "dB")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Crop, "Crop", "Transform", false, true, {}};
        d.params = {num("left", "Left", 0, 0, 100, 0.5, 1, "%"), num("right", "Right", 0, 0, 100, 0.5, 1, "%"),
                    num("top", "Top", 0, 0, 100, 0.5, 1, "%"), num("bottom", "Bottom", 0, 0, 100, 0.5, 1, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Color, "Color Correction", "Color", false, true, {}};
        d.params = {num("exposure", "Exposure", 0, -4, 4, 0.05, 2, "stops"),
                    num("contrast", "Contrast", 0, -100, 100, 1, 0),
                    num("highlights", "Highlights", 0, -100, 100, 1, 0),
                    num("shadows", "Shadows", 0, -100, 100, 1, 0),
                    num("saturation", "Saturation", 100, 0, 200, 1, 0),
                    num("temperature", "Temperature", 0, -100, 100, 1, 0),
                    num("tint", "Tint", 0, -100, 100, 1, 0)};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Lut, "LUT", "Color", false, true, {}};
        d.params = {num("intensity", "Intensity", 100, 0, 100, 1, 0, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Flip, "Flip", "Transform", false, true, {}};
        d.params = {enumeration("horizontal", "Horizontal", {"Off", "On"}), enumeration("vertical", "Vertical", {"Off", "On"})};
        r.push_back(d);
    }
    {
        EffectDef d{fx::BlackWhite, "Black & White", "Color", false, true, {}};
        d.params = {num("amount", "Amount", 100, 0, 100, 1, 0, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Blur, "Gaussian Blur", "Blur & Sharpen", false, true, {}};
        d.params = {num("radius", "Blurriness", 10, 0, 100, 0.5, 1, "px")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Sharpen, "Sharpen", "Blur & Sharpen", false, true, {}};
        d.params = {num("amount", "Amount", 50, 0, 200, 1, 0, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Vignette, "Vignette", "Stylize", false, true, {}};
        d.params = {num("amount", "Amount", 50, -100, 100, 1, 0, "%"), num("midpoint", "Midpoint", 50, 0, 100, 1, 0, "%"),
                    num("feather", "Feather", 50, 1, 100, 1, 0, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Typewriter, "Typewriter", "Text", false, true, {}};
        d.params = {num("progress", "Revealed", 100, 0, 100, 0.5, 1, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Mosaic, "Mosaic", "Stylize", false, true, {}};
        d.params = {num("size", "Block Size", 24, 2, 400, 1, 0, "px")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Glow, "Glow", "Stylize", false, true, {}};
        d.params = {num("threshold", "Threshold", 70, 0, 100, 1, 0, "%"), num("intensity", "Intensity", 100, 0, 300, 1, 0, "%"),
                    num("radius", "Radius", 24, 1, 100, 0.5, 1, "px")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Rounded, "Rounded Corners", "Stylize", false, true, {}};
        d.params = {num("radius", "Corner Radius", 48, 0, 1000, 1, 0, "px"), num("border", "Border Width", 0, 0, 100, 0.5, 1, "px"),
                    enumeration("color", "Border Color", {"White", "Black", "Blue", "Green", "Red", "Orange", "Yellow", "Violet"})};
        r.push_back(d);
    }

    {
        EffectDef d{fx::DropShadow, "Drop Shadow", "Stylize", false, true, {}};
        d.params = {num("opacity", "Opacity", 60, 0, 100, 1, 0, "%"), num("direction", "Direction", 135, 0, 360, 1, 0, "°"),
                    num("distance", "Distance", 16, 0, 300, 0.5, 1, "px"), num("softness", "Softness", 30, 0, 300, 0.5, 1, "px")};
        r.push_back(d);
    }

    {
        EffectDef d{fx::ChromaKey, "Chroma Key", "Keying", false, true, {}};
        d.params = {enumeration("key", "Key Color", {"Green", "Blue"}), num("tolerance", "Tolerance", 35, 0, 100, 0.5, 1, "%"),
                    num("softness", "Softness", 10, 0, 100, 0.5, 1, "%"), num("spill", "Spill Suppression", 50, 0, 100, 1, 0, "%")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Mask, "Shape Mask", "Keying", false, true, {}};
        d.params = {enumeration("shape", "Shape", {"Ellipse", "Rectangle"}), num("cx", "Center X", 50, -50, 150, 0.1, 1, "%"),
                    num("cy", "Center Y", 50, -50, 150, 0.1, 1, "%"), num("w", "Width", 50, 0, 200, 0.1, 1, "%"),
                    num("h", "Height", 50, 0, 200, 0.1, 1, "%"), num("feather", "Feather", 20, 0, 500, 0.5, 1, "px"),
                    enumeration("invert", "Invert", {"Off", "On"})};
        r.push_back(d);
    }

    // audio (video = false): processed per clip before Volume
    {
        EffectDef d{fx::HighPass, "High-Pass Filter", "Filter and EQ", false, false, {}};
        d.params = {num("frequency", "Cutoff", 80, 20, 1000, 1, 0, "Hz")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::LowPass, "Low-Pass Filter", "Filter and EQ", false, false, {}};
        d.params = {num("frequency", "Cutoff", 12000, 1000, 20000, 10, 0, "Hz")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Eq, "Parametric EQ", "Filter and EQ", false, false, {}};
        d.params = {num("low", "Low (120 Hz)", 0, -15, 15, 0.1, 1, "dB"), num("mid", "Mid", 0, -15, 15, 0.1, 1, "dB"),
                    num("midFreq", "Mid Frequency", 1000, 100, 10000, 10, 0, "Hz"),
                    num("high", "High (8 kHz)", 0, -15, 15, 0.1, 1, "dB")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Compressor, "Compressor", "Dynamics", false, false, {}};
        d.params = {num("threshold", "Threshold", -18, -60, 0, 0.5, 1, "dB"), num("ratio", "Ratio", 4, 1, 20, 0.1, 1, ":1"),
                    num("attack", "Attack", 10, 0.1, 100, 0.1, 1, "ms"), num("release", "Release", 150, 10, 1000, 1, 0, "ms"),
                    num("makeup", "Makeup Gain", 0, 0, 24, 0.1, 1, "dB")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::Denoise, "DeNoise", "Noise Reduction", false, false, {}};
        d.params = {num("reduction", "Reduction", 12, 0, 40, 0.5, 1, "dB")};
        r.push_back(d);
    }
    {
        EffectDef d{fx::NoiseGate, "Noise Gate", "Dynamics", false, false, {}};
        d.params = {num("threshold", "Threshold", -45, -80, 0, 0.5, 1, "dB"), num("release", "Release", 150, 10, 1000, 1, 0, "ms"),
                    num("reduction", "Reduction", -60, -80, 0, 1, 0, "dB")};
        r.push_back(d);
    }
    return r;
}
} // namespace

const ParamDef* EffectDef::find(const std::string& id) const {
    for (const auto& p : params)
        if (p.id == id) return &p;
    return nullptr;
}

const std::vector<EffectDef>& effectRegistry() {
    static const std::vector<EffectDef> r = buildRegistry();
    return r;
}

const EffectDef* findEffectDef(const std::string& type) {
    for (const auto& d : effectRegistry())
        if (d.type == type) return &d;
    return nullptr;
}

Effect makeEffect(const std::string& type) {
    Effect e;
    e.type = type;
    if (const EffectDef* d = findEffectDef(type))
        for (const auto& p : d->params) e.params[p.id].value = p.def;
    return e;
}

double Effect::p(const std::string& id, Tick local) const {
    auto it = params.find(id);
    if (it != params.end()) return it->second.at(local);
    if (const EffectDef* d = findEffectDef(type))
        if (const ParamDef* pd = d->find(id)) return pd->def;
    return 0;
}

CaptionStyle defaultCaptionStyle() {
    CaptionStyle s;
    s.text.size = 56;
    s.text.weight = 600;
    s.text.stroke = true;
    s.text.strokeWidth = 4;
    s.text.shadow = false;
    s.text.box = false;
    s.text.boxOpacity = 70;
    s.text.boxPadding = 12;
    return s;
}

// ---------------------------------------------------------------------------------------------
// Clip
Tick Clip::sourceSpan() const {
    if (isHold()) return 0;
    if (speed == 1.0) return duration;
    return Tick(std::llround(double(duration) * speed));
}

Tick Clip::sourceAt(Tick t, Tick frameDur) const {
    Tick local = std::clamp<Tick>(t - start, 0, std::max<Tick>(0, duration - 1));
    if (isHold()) return sourceIn;
    if (reverse) {
        Tick mirrored = std::max<Tick>(0, duration - std::max<Tick>(1, frameDur) - local);
        Tick off = speed == 1.0 ? mirrored : Tick(std::llround(double(mirrored) * speed));
        return sourceIn + off;
    }
    Tick off = speed == 1.0 ? local : Tick(std::llround(double(local) * speed));
    return sourceIn + off;
}

const char* transitionName(TransitionType t) {
    switch (t) {
    case TransitionType::CrossDissolve: return "Cross Dissolve";
    case TransitionType::DipToBlack: return "Dip to Black";
    case TransitionType::DipToWhite: return "Dip to White";
    case TransitionType::WipeLeft: return "Wipe Left";
    case TransitionType::WipeRight: return "Wipe Right";
    case TransitionType::WipeUp: return "Wipe Up";
    case TransitionType::WipeDown: return "Wipe Down";
    case TransitionType::Slide: return "Slide";
    case TransitionType::ConstantPower: return "Constant Power";
    case TransitionType::Push: return "Push";
    case TransitionType::CrossZoom: return "Cross Zoom";
    case TransitionType::IrisRound: return "Iris Round";
    case TransitionType::BlurDissolve: return "Blur Dissolve";
    case TransitionType::Whip: return "Whip";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// Sequence
Tick Sequence::duration() const {
    Tick d = 0;
    for (const auto& c : clips) d = std::max(d, c.end());
    for (const auto& c : captions) d = std::max(d, c.end);
    return d;
}

Track* Sequence::track(Id id) {
    for (auto& t : tracks)
        if (t.id == id) return &t;
    return nullptr;
}
const Track* Sequence::track(Id id) const { return const_cast<Sequence*>(this)->track(id); }

Clip* Sequence::clip(Id id) {
    for (auto& c : clips)
        if (c.id == id) return &c;
    return nullptr;
}
const Clip* Sequence::clip(Id id) const { return const_cast<Sequence*>(this)->clip(id); }

Transition* Sequence::transition(Id id) {
    for (auto& t : transitions)
        if (t.id == id) return &t;
    return nullptr;
}

std::vector<const Track*> Sequence::tracksOf(TrackKind k) const {
    std::vector<const Track*> r;
    for (const auto& t : tracks)
        if (t.kind == k) r.push_back(&t);
    return r;
}

int Sequence::trackIndexInKind(Id trackId) const {
    const Track* t = track(trackId);
    if (!t) return -1;
    int i = 0;
    for (const auto& o : tracks) {
        if (o.kind != t->kind) continue;
        if (o.id == trackId) return i;
        ++i;
    }
    return -1;
}

const Track* Sequence::trackAt(TrackKind k, int index) const {
    int i = 0;
    for (const auto& t : tracks) {
        if (t.kind != k) continue;
        if (i == index) return &t;
        ++i;
    }
    return nullptr;
}

std::vector<const Clip*> Sequence::clipsOnTrack(Id trackId) const {
    std::vector<const Clip*> r;
    for (const auto& c : clips)
        if (c.trackId == trackId) r.push_back(&c);
    std::sort(r.begin(), r.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    return r;
}

std::vector<Id> Sequence::linkedClips(Id clipId) const {
    const Clip* c = clip(clipId);
    if (!c) return {};
    if (!c->linkId) return {clipId};
    std::vector<Id> r;
    for (const auto& o : clips)
        if (o.linkId == c->linkId) r.push_back(o.id);
    return r;
}

const Transition* Sequence::transitionAtCut(Id a, Id b) const {
    for (const auto& t : transitions)
        if (t.clipA == a && t.clipB == b) return &t;
    return nullptr;
}

Sequence makeDefaultSequence(const SequenceSettings& s) {
    Sequence seq;
    seq.uid = 1;  // a project's first sequence; others get Project::nextSequenceUid
    seq.settings = s;
    for (int i = 0; i < 3; ++i) {
        Track t;
        t.id = seq.newId();
        t.kind = TrackKind::Video;
        t.name = "V" + std::to_string(i + 1);
        seq.tracks.push_back(t);
    }
    for (int i = 0; i < 3; ++i) {
        Track t;
        t.id = seq.newId();
        t.kind = TrackKind::Audio;
        t.name = "A" + std::to_string(i + 1);
        seq.tracks.push_back(t);
    }
    Track c;
    c.id = seq.newId();
    c.kind = TrackKind::Caption;
    c.name = "C1";
    c.height = 32;
    seq.tracks.push_back(c);
    return seq;
}

Asset* Project::asset(Id id) {
    for (auto& a : assets)
        if (a.id == id) return &a;
    return nullptr;
}
const Asset* Project::asset(Id id) const { return const_cast<Project*>(this)->asset(id); }

void initBuiltinEffects(Clip& c, TrackKind kind) {
    if (kind == TrackKind::Audio) {
        c.volume = makeEffect(fx::Volume);
    } else {
        c.motion = makeEffect(fx::Motion);
        c.opacity = makeEffect(fx::Opacity);
    }
}

const Sequence* Project::sequenceByUid(Id uid) const {
    if (sequence.uid == uid) return &sequence;
    for (const auto& s : otherSequences)
        if (s.uid == uid) return &s;
    return nullptr;
}

Clip makeVideoClip(const Asset& a, Id trackId, Tick start, Tick sourceIn, Tick duration) {
    Clip c;
    c.trackId = trackId;
    c.kind = ClipKind::Media;
    c.assetId = a.id;
    c.name = a.name;
    c.start = start;
    c.sourceIn = sourceIn;
    c.duration = duration;
    initBuiltinEffects(c, TrackKind::Video);
    return c;
}

Clip makeAudioClip(const Asset& a, int stream, Id trackId, Tick start, Tick sourceIn, Tick duration) {
    Clip c;
    c.trackId = trackId;
    c.kind = ClipKind::Media;
    c.assetId = a.id;
    c.stream = stream;
    c.name = a.name;
    if (a.info.audio.size() > 1) {
        const auto& s = a.info.audio[size_t(stream)];
        c.name += s.title.empty() ? " [" + std::to_string(stream + 1) + "]" : " [" + s.title + "]";
    }
    c.start = start;
    c.sourceIn = sourceIn;
    c.duration = duration;
    initBuiltinEffects(c, TrackKind::Audio);
    return c;
}

Clip makeTextClip(Id trackId, Tick start, Tick duration, const std::string& text) {
    Clip c;
    c.trackId = trackId;
    c.kind = ClipKind::Text;
    c.name = "Title";
    c.text = text;
    c.start = start;
    c.duration = duration;
    c.fit = FitMode::None;
    initBuiltinEffects(c, TrackKind::Video);
    return c;
}

Clip makeMatteClip(Id trackId, Tick start, Tick duration, uint32_t argb) {
    Clip c;
    c.trackId = trackId;
    c.kind = ClipKind::Matte;
    c.name = "Color Matte";
    c.matteColor = argb;
    c.start = start;
    c.duration = duration;
    initBuiltinEffects(c, TrackKind::Video);
    return c;
}

Clip makeAdjustmentClip(Id trackId, Tick start, Tick duration) {
    Clip c;
    c.trackId = trackId;
    c.kind = ClipKind::Adjustment;
    c.name = "Adjustment Layer";
    c.start = start;
    c.duration = duration;
    initBuiltinEffects(c, TrackKind::Video);
    return c;
}

TickRange transitionRange(const Sequence& s, const Transition& tr) {
    const Clip* a = tr.clipA ? s.clip(tr.clipA) : nullptr;
    const Clip* b = tr.clipB ? s.clip(tr.clipB) : nullptr;
    Tick d = tr.duration;
    if (a && b) {
        Tick cut = b->start;
        switch (tr.align) {
        case TransitionAlign::Center: {
            Tick half = floorToFrame(d / 2, s.settings.fps);  // odd frame counts lean right
            return {cut - half, cut - half + d};
        }
        case TransitionAlign::Start: return {cut, cut + d};
        case TransitionAlign::End: return {cut - d, cut};
        }
    }
    if (b) return {b->start, b->start + d};
    if (a) return {a->end() - d, a->end()};
    return {0, 0};
}

double dbToGain(double db) { return db <= -96.0 ? 0.0 : std::pow(10.0, db / 20.0); }
double gainToDb(double g) { return g <= 1e-6 ? -120.0 : 20.0 * std::log10(g); }

} // namespace cut
