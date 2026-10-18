#pragma once
// Video/audio encoders + muxer used by export and proxy generation.
// Video input paths (auto-selected, fastest first):
//   1. NVENC fed with D3D11 textures (zero copy: our NV12 render target is GPU-copied into the
//      encoder's frame pool),
//   2. NVENC from system memory (GPU readback of NV12),
//   3. libx264 from system memory.
#include "render/gpu.h"

#include <functional>
#include <memory>
#include <string>

struct AVPacket;
struct AVCodecContext;
struct AVFormatContext;
struct AVStream;

namespace cut {

struct VideoEncodeConfig {
    std::string codec = "h264_nvenc";  // h264_nvenc, hevc_nvenc, av1_nvenc, libx264
    int width = 1920, height = 1080;
    Rational fps{60, 1};
    Rational timeBase{0, 1};  // pts units; default 1/fps (frame index)
    int64_t bitrate = 16'000'000;
    int64_t maxBitrate = 24'000'000;
    int cq = 0;            // > 0: constant-quality mode
    std::string preset = "p5";
    bool tuneHq = true;
    int gop = 120;          // frames; 1 = all-intra
    int bframes = 2;
    int lookahead = 16;
    bool zeroCopy = true;   // try D3D11 frame input
    ColorMatrix matrix = ColorMatrix::BT709;
};

class VideoEncoder {
public:
    using PacketFn = std::function<bool(AVPacket*)>;
    VideoEncoder();
    ~VideoEncoder();
    bool open(const VideoEncodeConfig& cfg, std::shared_ptr<GpuDevice> gpu, std::string& error);
    void setPacketSink(PacketFn f) { sink_ = std::move(f); }
    bool zeroCopy() const { return zeroCopy_; }
    bool hardware() const { return hardware_; }
    std::string describe() const;
    AVCodecContext* context() const;

    // Zero-copy path: nv12 is our render target (same device). pts in the config time base.
    bool encodeTexture(const GpuTexture& nv12, int64_t pts);
    // System-memory path: NV12 planes (Y pitch, then UV plane).
    bool encodeNV12(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int64_t frameIndex);
    bool flush();
    int64_t framesSent() const { return sent_; }

private:
    bool drain();
    struct Impl;
    std::unique_ptr<Impl> d_;
    PacketFn sink_;
    bool zeroCopy_ = false, hardware_ = false;
    int64_t sent_ = 0;
};

class AudioEncoder {
public:
    using PacketFn = std::function<bool(AVPacket*)>;
    AudioEncoder();
    ~AudioEncoder();
    // 48 kHz stereo; codec: "aac" (AAC-LC), "libmp3lame", "flac", "pcm_s16le" (WAV)
    bool open(int bitrate, std::string& error, const std::string& codec = "aac");
    void setPacketSink(PacketFn f) { sink_ = std::move(f); }
    AVCodecContext* context() const;
    // Interleaved float stereo; buffers internally to the codec frame size.
    bool encode(const float* interleaved, int frames);
    bool flush();

private:
    bool sendFrame(bool final);
    struct Impl;
    std::unique_ptr<Impl> d_;
    PacketFn sink_;
};

class Muxer {
public:
    Muxer();
    ~Muxer();
    bool open(const std::string& path, std::string& error);  // container from extension (mp4/mkv/mov)
    int addStream(AVCodecContext* enc, bool faststart);
    bool writeHeader(std::string& error);
    // Rescales from the encoder time base and writes interleaved. Thread-safe.
    bool write(AVPacket* pkt, int stream, AVCodecContext* enc);
    bool finish(std::string& error);
    void abort();  // close and delete the partial file

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Encoders available in this FFmpeg build that also open on this machine (cached).
bool encoderUsable(const std::string& name, std::shared_ptr<GpuDevice> gpu);

} // namespace cut
