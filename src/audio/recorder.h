#pragma once
// Voice-over recording: captures an input device (default microphone) with WASAPI in shared mode
// and writes 48 kHz float stereo WAV as it goes. `loopback` captures what a render device plays
// instead (used by tests, which must never open the user's microphone).
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cut {

class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();
    // deviceId: capture endpoint (empty = default microphone) or, with loopback, a render endpoint.
    bool start(const std::string& wavPath, const std::string& deviceId, bool loopback, std::string& error);
    void stop();  // finishes the WAV file
    bool recording() const { return running_.load(); }
    int64_t frames() const { return frames_.load(); }      // 48 kHz frames written
    float peak();                                           // since the last call (meter)
    int64_t firstSampleUs() const { return firstUs_.load(); }  // wall clock (QPC us) of the first frame
    std::string deviceName() const { return deviceName_; }

private:
    void run(std::string deviceId, bool loopback);
    std::thread thread_;
    std::atomic<bool> running_{false}, quit_{false}, ready_{false}, failed_{false};
    std::atomic<int64_t> frames_{0}, firstUs_{0};
    std::atomic<float> peak_{0};
    std::string path_, error_, deviceName_;
};

// Capture endpoints (id, name) for a device picker.
std::vector<std::pair<std::string, std::string>> listCaptureDevices();

} // namespace cut
