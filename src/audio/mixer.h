#pragma once
// Sequence audio mixer: clip gain + volume keyframes + fades + constant-power crossfades,
// per-track fader/pan/mute/solo, master fader, peak meters. Used by the playback mixer thread and
// offline by export (one Mixer instance per consumer; not thread-safe itself).
#include "audio/audio_fx.h"
#include "audio/clip_audio.h"
#include "audio/loudness.h"
#include "engine/snapshot.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cut {

struct MeterSnapshot {
    std::vector<std::pair<Id, std::array<float, 2>>> tracks;  // peak linear per channel
    std::array<float, 2> master{0, 0};
    // master loudness (LUFS; -inf = silence / not enough audio yet)
    double momentary = -1e9, shortTerm = -1e9, integrated = -1e9;
};

class Mixer {
public:
    // Renders timeline samples [ts, ts+n) (48 kHz) into out (interleaved stereo).
    void render(const RenderSnapshot& snap, int64_t ts, float* out, int n);
    // Peaks since the last call (thread-safe; UI polls at ~30 Hz).
    MeterSnapshot takeMeters();
    void reset();  // drop clip readers (e.g. after a big seek)
    void resetLoudness();  // new playback session: integrated loudness starts over
    // Drop readers for clips not rendered in the last `seconds` of mixing.
    void prune();

private:
    struct ClipState {
        std::unique_ptr<ClipAudio> audio;
        bool failed = false;
        int64_t lastUsed = 0;
        ClipFxChain fx;            // clip audio effects (state across blocks)
        int64_t nextSample = -1;  // where the last block ended
        std::unique_ptr<Mixer> sub;  // nested sequence clips: their own mixer
        Id asset = 0;                // media of audio (for handing the reader on)
        int stream = -1;
        int64_t endSample = INT64_MAX;  // timeline sample where the clip's audio ends
        int64_t renderedTo = INT64_MIN;  // end of the last block it rendered

    };
    ClipAudio* clipAudio(const RenderSnapshot& snap, const Clip& c, int64_t from);
    struct DuckState {
        double env = 0, gain = 1;
        int hold = 0;
        int64_t next = -1;
    };
    void duck(Id track, const float* key, double duckDb, int64_t ts, float* buf, int n);
    std::unordered_map<Id, DuckState> ducks_;
    std::unordered_map<Id, std::vector<float>> trackOut_;
    std::unordered_map<Id, ClipState> clips_;
    std::vector<float> trackBuf_, clipBuf_;
    std::vector<double> gains_;
    int64_t blockCounter_ = 0;
    std::mutex meterMutex_;
    LoudnessMeter loud_;  // master bus (mixing thread only)
    int loudSteps_ = 0;
    std::atomic<bool> loudReset_{false};
    MeterSnapshot meters_;
};

// Gain of a clip at clip-local time (volume keyframes, clip gain, audio fades) - linear.
double clipGainAt(const Clip& c, Tick local);

} // namespace cut
