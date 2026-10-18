#pragma once
// Synchronous video decoder producing GPU frames. Hardware (D3D11VA / NVDEC) on the shared
// device when the codec/profile allows, otherwise multithreaded software decode + upload.
// Decoded hardware surfaces are copied into our own pooled textures immediately so the
// decoder's surface pool never stalls while frames sit in caches.
#include "render/gpu.h"

#include <memory>
#include <string>
#include <vector>

struct AVBufferRef;

namespace cut {

AVBufferRef* ffHwDeviceFor(GpuDevice& g);  // shared AVD3D11VADeviceContext for this device

struct DecodedPicture {
    Tick pts = 0;
    Tick duration = 0;
    GpuFramePtr frame;  // null when decoded with keep=false
};

class VideoDecoder {
public:
    struct Options {
        bool allowHw = true;
        int swThreads = 0;  // 0 = auto
    };
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // origin: MediaInfo::origin (raw container ticks that map to media time 0).
    bool open(const std::string& path, std::shared_ptr<GpuDevice> gpu, Tick origin, const Options& opt,
              std::string& error);
    void close();
    bool isOpen() const;
    bool isHw() const { return hw_; }
    int width() const { return width_; }
    int height() const { return height_; }
    Tick frameDurationHint() const { return frameDur_; }
    std::string describe() const;

    // Positions the demuxer at the keyframe at or before t (media time).
    bool seek(Tick t);
    // Next frame in presentation order. Frames with pts < keepFrom skip the GPU copy (frames being
    // skipped while decoding forward to a seek target). Returns false at end of stream.
    bool next(Tick keepFrom, DecodedPicture& out);
    bool atEof() const { return eof_; }

    // Stats
    uint64_t framesDecoded() const { return decoded_; }

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    bool hw_ = false;
    bool eof_ = false;
    int width_ = 0, height_ = 0;
    Tick frameDur_ = kTicksPerSecond / 30;
    uint64_t decoded_ = 0;
};

} // namespace cut
