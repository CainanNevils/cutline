#pragma once
// Audio device output: 48 kHz stereo float pulled by a real-time thread.
// Primary: WASAPI shared mode, event driven, own MMCSS thread. Fallback: Qt QAudioSink.
// The device's played position is the playback master clock.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cut {

class AudioOutput {
public:
    // Called on the audio thread; must be real-time safe (no locks that can block, no allocation).
    using PullFn = std::function<void(float* interleaved, int frames)>;
    virtual ~AudioOutput() = default;
    virtual bool start(PullFn pull, std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    // Frames heard so far (device clock, interpolated). Monotonic while running.
    virtual int64_t playedFrames() const = 0;
    virtual double latencyMs() const = 0;
    virtual std::string name() const = 0;
    virtual uint64_t glitches() const = 0;
    virtual double bufferMs() const = 0;
    // Frames queued (silence) before the first pull; playedFrames() counts them too.
    virtual int64_t prefillFrames() const = 0;
};

// deviceId: WASAPI endpoint id (empty = default device). Tests set CUTLINE_AUDIO_DEVICE so test
// tones never play on the user's speakers.
std::unique_ptr<AudioOutput> createWasapiOutput(const std::string& deviceId = {});
std::unique_ptr<AudioOutput> createQtAudioOutput();
// WASAPI, else Qt fallback. `backend` receives "WASAPI" or "Qt".
std::unique_ptr<AudioOutput> createAudioOutput(AudioOutput::PullFn pull, std::string& backend, std::string& error,
                                               const std::string& deviceId = {});
// Output endpoints: (id, friendly name).
std::vector<std::pair<std::string, std::string>> listAudioDevices();
// Id of the current default output endpoint (empty if none).
std::string defaultAudioDeviceId();

} // namespace cut
