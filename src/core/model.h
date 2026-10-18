#pragma once
// Project data model. Pure C++: no Qt, no FFmpeg. Everything here is a plain value type so the
// undo system can diff copies and the engine can read immutable snapshots on other threads.
#include "core/time.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cut {

using Id = uint64_t;

// ---------------------------------------------------------------------------------------------
// Animatable parameters
enum class Interp : uint8_t { Linear = 0, Hold = 1, Ease = 2, EaseIn = 3, EaseOut = 4 };  // (stored as numbers)

struct Keyframe {
    Tick t = 0;  // clip-local time (0 = clip start on the timeline)
    double v = 0;
    Interp interp = Interp::Linear;  // interpolation of the segment that starts at this key
    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

struct Param {
    double value = 0;            // used when there are no keyframes
    std::vector<Keyframe> keys;  // sorted by t, unique t
    bool animated() const { return !keys.empty(); }
    double at(Tick local) const;
    // Sets the value at `local`: updates/creates a key when animated, else the static value.
    void set(Tick local, double v);
    void addKey(Tick local, double v, Interp interp = Interp::Linear);
    bool removeKeyAt(Tick local, Tick tolerance = 0);
    const Keyframe* keyAt(Tick local, Tick tolerance = 0) const;
    friend bool operator==(const Param&, const Param&) = default;
};

// ---------------------------------------------------------------------------------------------
// Effects. Types are strings so the project file stays readable and forward-compatible.
namespace fx {
inline constexpr const char* Motion = "motion";
inline constexpr const char* Opacity = "opacity";
inline constexpr const char* Crop = "crop";
inline constexpr const char* Color = "color";
inline constexpr const char* Lut = "lut";
inline constexpr const char* Volume = "volume";
inline constexpr const char* Flip = "flip";
inline constexpr const char* Blur = "blur";
inline constexpr const char* Sharpen = "sharpen";
inline constexpr const char* BlackWhite = "blackwhite";
inline constexpr const char* Vignette = "vignette";
inline constexpr const char* Rounded = "rounded";  // rounded corners + border (picture in picture)
inline constexpr const char* DropShadow = "dropshadow";
inline constexpr const char* ChromaKey = "chromakey";
inline constexpr const char* Mask = "mask";
inline constexpr const char* Glow = "glow";
inline constexpr const char* Mosaic = "mosaic";
inline constexpr const char* Typewriter = "typewriter";  // text clips: reveal characters
// audio clip effects
inline constexpr const char* HighPass = "highpass";
inline constexpr const char* LowPass = "lowpass";
inline constexpr const char* Eq = "eq";
inline constexpr const char* Compressor = "compressor";
inline constexpr const char* NoiseGate = "noisegate";
inline constexpr const char* Denoise = "denoise";
} // namespace fx

enum class BlendMode : uint8_t { Normal = 0, Add = 1, Multiply = 2, Screen = 3 };

struct Effect {
    std::string type;
    bool enabled = true;
    std::map<std::string, Param> params;
    std::string path;  // LUT file for "lut"
    double p(const std::string& id, Tick local) const;  // param value or registry default
    friend bool operator==(const Effect&, const Effect&) = default;
};

struct ParamDef {
    std::string id, label, unit;
    double def = 0, min = 0, max = 1, step = 1;
    int decimals = 1;
    bool animatable = true;
    std::vector<std::string> enumLabels;  // non-empty => enum (non-animatable)
    bool isBool = false;
};

struct EffectDef {
    std::string type, label, category;  // category: "Video", "Audio", "Built-in"
    bool builtin = false;
    bool video = true;
    std::vector<ParamDef> params;
    const ParamDef* find(const std::string& id) const;
};

const std::vector<EffectDef>& effectRegistry();
const EffectDef* findEffectDef(const std::string& type);
Effect makeEffect(const std::string& type);  // all params at defaults

// ---------------------------------------------------------------------------------------------
// Text (titles and caption style)
struct TextStyle {
    std::string font = "Segoe UI";
    double size = 96;
    int weight = 700;  // QFont-like 100..900
    bool italic = false;
    uint32_t fill = 0xFFFFFFFF;  // ARGB
    bool stroke = false;
    double strokeWidth = 6;
    uint32_t strokeColor = 0xFF000000;
    bool shadow = true;
    double shadowX = 4, shadowY = 4, shadowBlur = 8;
    uint32_t shadowColor = 0xC0000000;
    bool box = false;
    uint32_t boxColor = 0xFF000000;
    double boxPadding = 24;
    double boxOpacity = 60;  // percent
    double boxRadius = 0;    // rounded box corners (px)
    int align = 1;           // 0 left, 1 center, 2 right
    double lineSpacing = 1.0;
    double tracking = 0;           // letter spacing, percent of the font size
    bool gradient = false;         // vertical fill gradient from fill (top) to fill2 (bottom)
    uint32_t fill2 = 0xFFFFC040;
    friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

struct CaptionStyle {
    TextStyle text;
    double marginBottom = 8;  // percent of frame height from the bottom (safe margin)
    friend bool operator==(const CaptionStyle&, const CaptionStyle&) = default;
};
CaptionStyle defaultCaptionStyle();

// ---------------------------------------------------------------------------------------------
// Media
enum class ColorRange : uint8_t { Unknown = 0, Limited = 1, Full = 2 };
enum class ColorMatrix : uint8_t { Unknown = 0, BT709 = 1, BT601 = 2, BT2020 = 3 };
enum class ColorTransfer : uint8_t { SDR = 0, PQ = 1, HLG = 2 };

struct VideoStreamInfo {
    int index = -1;  // container stream index
    int width = 0, height = 0;
    Rational fps{0, 1};       // average frame rate
    Rational timeBase{1, 1};
    std::string codec, pixFmt;
    int bitDepth = 8;
    ColorRange range = ColorRange::Unknown;
    ColorMatrix matrix = ColorMatrix::Unknown;
    ColorTransfer transfer = ColorTransfer::SDR;
    bool vfr = false;
    bool hasAlpha = false;
    Tick duration = 0;
    Tick startTime = 0;  // first pts (ticks)
    friend bool operator==(const VideoStreamInfo&, const VideoStreamInfo&) = default;
};

struct AudioStreamInfo {
    int index = -1;
    int sampleRate = 0, channels = 0;
    std::string codec, title, language;
    Tick duration = 0;
    Tick startTime = 0;
    friend bool operator==(const AudioStreamInfo&, const AudioStreamInfo&) = default;
};

struct MediaInfo {
    bool valid = false;
    std::string error;
    std::string container;
    bool isImage = false;
    bool hasVideo = false;
    Tick duration = 0;
    Tick origin = 0;  // raw container time that maps to media time 0 (earliest stream start)
    VideoStreamInfo video;
    std::vector<AudioStreamInfo> audio;
    int64_t fileSize = 0;
    int64_t mtime = 0;  // seconds since epoch
    friend bool operator==(const MediaInfo&, const MediaInfo&) = default;
};

struct Asset {
    Id id = 0;
    Id binId = 0;  // 0 = root
    std::string name;
    std::string path;     // absolute
    std::string relPath;  // relative to the project file (for relinking moved projects)
    MediaInfo info;
    bool offline = false;  // runtime: file missing / changed
    friend bool operator==(const Asset&, const Asset&) = default;
};

struct Bin {
    Id id = 0;
    Id parent = 0;
    std::string name;
    friend bool operator==(const Bin&, const Bin&) = default;
};

// ---------------------------------------------------------------------------------------------
// Sequence
enum class TrackKind : uint8_t { Video = 0, Audio = 1, Caption = 2 };

struct Track {
    Id id = 0;
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool locked = false;
    bool muted = false;    // audio mute
    bool solo = false;     // audio solo
    bool visible = true;   // video eye / caption visibility
    int height = 56;
    double volumeDb = 0;   // audio track fader
    double pan = 0;        // -1..1
    Id duckUnder = 0;      // audio: lower this track while that track has signal (0 = off)
    double duckDb = -12;   // how far (dB)
    friend bool operator==(const Track&, const Track&) = default;
};

// Nested: another sequence as a clip. Matte: solid colour. Adjustment: its effects apply to the
// picture of every track below it.
enum class ClipKind : uint8_t { Media = 0, Text = 1, Nested = 2, Matte = 3, Adjustment = 4 };
enum class FitMode : uint8_t { Fit = 0, Fill = 1, None = 2 };

struct Clip {
    Id id = 0;
    Id trackId = 0;
    ClipKind kind = ClipKind::Media;
    Id assetId = 0;
    Id nestedSeq = 0;  // Nested clips: Sequence::uid of the sequence shown
    uint32_t matteColor = 0xFF1E1E1E;  // Matte clips (ARGB)
    int stream = 0;  // audio clips: index into Asset::info.audio; video: unused
    std::string name;

    Tick start = 0;     // timeline position
    Tick duration = 0;  // timeline duration
    Tick sourceIn = 0;  // source time at the first timeline tick (forward clips)

    double speed = 1.0;  // 0 = frame hold
    bool reverse = false;
    bool frameBlend = false;
    bool enabled = true;
    Id linkId = 0;  // clips sharing a non-zero linkId move/select together
    FitMode fit = FitMode::Fit;

    Effect motion;   // video/text: built-in transform
    Effect opacity;  // video/text: built-in opacity + blend
    Effect volume;   // audio: built-in level (dB)
    std::vector<Effect> effects;

    double gainDb = 0;  // audio clip gain
    uint32_t label = 0;  // ARGB label colour in the timeline (0 = by kind)
    Tick fadeIn = 0, fadeOut = 0;  // audio fades / title fades

    std::string text;  // text clips
    TextStyle textStyle;

    Tick end() const { return start + duration; }
    bool isHold() const { return speed == 0.0; }
    // Source media span consumed by this clip: [sourceIn, sourceOut).
    Tick sourceSpan() const;
    Tick sourceOut() const { return sourceIn + sourceSpan(); }
    // Source time shown at timeline time t (t clamped into the clip). frameDur = sequence frame
    // duration; reverse clips mirror whole frames so frame k shows forward frame N-1-k.
    Tick sourceAt(Tick t, Tick frameDur = 1) const;
    bool contains(Tick t) const { return t >= start && t < end(); }
    friend bool operator==(const Clip&, const Clip&) = default;
};

enum class TransitionType : uint8_t {
    CrossDissolve = 0,
    DipToBlack,
    DipToWhite,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    Slide,
    ConstantPower,  // audio crossfade
    // later video transitions (appended: values are stored in projects)
    Push,
    CrossZoom,
    IrisRound,
    BlurDissolve,
    Whip,
};
enum class TransitionAlign : uint8_t { Center = 0, Start = 1, End = 2 };
const char* transitionName(TransitionType t);

struct Transition {
    Id id = 0;
    Id trackId = 0;
    Id clipA = 0;  // outgoing clip (0 = transition on clipB's head only)
    Id clipB = 0;  // incoming clip (0 = transition on clipA's tail only)
    TransitionType type = TransitionType::CrossDissolve;
    Tick duration = kTicksPerSecond;
    TransitionAlign align = TransitionAlign::Center;
    friend bool operator==(const Transition&, const Transition&) = default;
};

struct Marker {
    Id id = 0;
    Tick t = 0;
    std::string name;
    uint32_t color = 0xFF3FB950;
    friend bool operator==(const Marker&, const Marker&) = default;
};

struct Caption {
    Id id = 0;
    Tick start = 0, end = 0;
    std::string text;
    friend bool operator==(const Caption&, const Caption&) = default;
};

struct SequenceSettings {
    int width = 2560, height = 1440;
    Rational fps{60, 1};
    int sampleRate = 48000;
    friend bool operator==(const SequenceSettings&, const SequenceSettings&) = default;
};

struct Sequence {
    std::string name = "Sequence 01";
    Id uid = 0;  // stable identity within the project (nested clips refer to it)
    SequenceSettings settings;
    std::vector<Track> tracks;  // V1..Vn, A1..An, C1 (stored by kind, V1 first)
    std::vector<Clip> clips;
    std::vector<Transition> transitions;
    std::vector<Marker> markers;
    std::vector<Caption> captions;
    CaptionStyle captionStyle = defaultCaptionStyle();
    Tick inPoint = kInvalidTick, outPoint = kInvalidTick;
    double masterVolumeDb = 0;
    Id nextId = 1;

    Id newId() { return nextId++; }
    Tick frameDur() const { return frameDuration(settings.fps); }
    Tick duration() const;  // end of the last clip/caption

    Track* track(Id id);
    const Track* track(Id id) const;
    Clip* clip(Id id);
    const Clip* clip(Id id) const;
    Transition* transition(Id id);
    std::vector<const Track*> tracksOf(TrackKind k) const;  // in V1..Vn order
    int trackIndexInKind(Id trackId) const;                // 0-based within its kind, -1 if none
    const Track* trackAt(TrackKind k, int index) const;
    std::vector<const Clip*> clipsOnTrack(Id trackId) const;  // sorted by start
    std::vector<Id> linkedClips(Id clipId) const;             // includes clipId
    const Transition* transitionAtCut(Id clipA, Id clipB) const;

    friend bool operator==(const Sequence&, const Sequence&) = default;
};

Sequence makeDefaultSequence(const SequenceSettings& s = {});

struct ProjectSettings {
    std::string cacheDir;  // empty = default (%LOCALAPPDATA%\Cutline\cache)
    friend bool operator==(const ProjectSettings&, const ProjectSettings&) = default;
};

struct Project {
    static constexpr int kSchemaVersion = 1;
    std::string name = "Untitled";
    ProjectSettings settings;
    std::vector<Bin> bins;
    std::vector<Asset> assets;
    Sequence sequence;                     // the open (active) sequence
    std::vector<Sequence> otherSequences;  // the project's other sequences (tab order without the active one)
    int activeSequence = 0;                // tab position of the active sequence
    Id nextAssetId = 1;
    Id nextSequenceUid = 2;                // (the first sequence is uid 1)

    const Sequence* sequenceByUid(Id uid) const;

    Asset* asset(Id id);
    const Asset* asset(Id id) const;
    Id newAssetId() { return nextAssetId++; }
};

// Helpers for creating clips from assets.
Clip makeVideoClip(const Asset& a, Id trackId, Tick start, Tick sourceIn, Tick duration);
Clip makeAudioClip(const Asset& a, int stream, Id trackId, Tick start, Tick sourceIn, Tick duration);
Clip makeTextClip(Id trackId, Tick start, Tick duration, const std::string& text);
Clip makeMatteClip(Id trackId, Tick start, Tick duration, uint32_t argb);
Clip makeAdjustmentClip(Id trackId, Tick start, Tick duration);
void initBuiltinEffects(Clip& c, TrackKind kind);
// Default duration of stills / titles.
constexpr Tick kDefaultStillDuration = 5 * kTicksPerSecond;

// Region covered by a transition on the timeline.
struct TickRange {
    Tick a = 0, b = 0;
    bool contains(Tick t) const { return t >= a && t < b; }
    Tick len() const { return b - a; }
};
TickRange transitionRange(const Sequence& s, const Transition& tr);

// Linear gain helpers.
double dbToGain(double db);
double gainToDb(double g);

} // namespace cut
