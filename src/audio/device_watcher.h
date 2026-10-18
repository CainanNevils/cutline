#pragma once
// Watches the Windows audio endpoints: calls `onChange` (on a system thread) when the default
// render device changes or an output device appears / disappears, so playback can follow
// headphones being plugged in or a USB interface being removed.
#include <functional>
#include <memory>

namespace cut {

class AudioDeviceWatcher {
public:
    explicit AudioDeviceWatcher(std::function<void()> onChange);
    ~AudioDeviceWatcher();
    AudioDeviceWatcher(const AudioDeviceWatcher&) = delete;
    AudioDeviceWatcher& operator=(const AudioDeviceWatcher&) = delete;
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace cut
