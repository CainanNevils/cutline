#include "export/encoder.h"

#include "media/ff.h"
#include "media/video_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace cut {

// ---------------------------------------------------------------------------------------------
struct VideoEncoder::Impl {
    VideoEncodeConfig cfg;
    std::shared_ptr<GpuDevice> gpu;
    ff::CodecPtr cc;
    ff::BufPtr frames;  // AVHWFramesContext (zero copy)
    ff::FramePtr swFrame;
    ff::PacketPtr pkt = ff::makePacket();
    std::string name;
};

VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() = default;
AVCodecContext* VideoEncoder::context() const { return d_ ? d_->cc.get() : nullptr; }

std::string VideoEncoder::describe() const {
    if (!d_) return "closed";
    return d_->name + (zeroCopy_ ? " (GPU frames)" : hardware_ ? " (system memory)" : " (CPU)");
}

static bool configureCommon(AVCodecContext* c, const VideoEncodeConfig& cfg, bool nvenc) {
    c->width = cfg.width;
    c->height = cfg.height;
    c->time_base = cfg.timeBase.valid() ? AVRational{int(cfg.timeBase.num), int(cfg.timeBase.den)}
                                        : AVRational{int(cfg.fps.den), int(cfg.fps.num)};
    c->framerate = AVRational{int(cfg.fps.num), int(cfg.fps.den)};
    c->sample_aspect_ratio = AVRational{1, 1};
    c->gop_size = cfg.gop;
    c->max_b_frames = cfg.gop == 1 ? 0 : cfg.bframes;
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = cfg.matrix == ColorMatrix::BT601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    c->color_primaries = cfg.matrix == ColorMatrix::BT601 ? AVCOL_PRI_SMPTE170M : AVCOL_PRI_BT709;
    c->color_trc = cfg.matrix == ColorMatrix::BT601 ? AVCOL_TRC_SMPTE170M : AVCOL_TRC_BT709;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (nvenc) {
        av_opt_set(c->priv_data, "preset", cfg.preset.c_str(), 0);
        if (cfg.tuneHq) av_opt_set(c->priv_data, "tune", "hq", 0);
        if (cfg.cq > 0) {
            av_opt_set(c->priv_data, "rc", "vbr", 0);
            av_opt_set_int(c->priv_data, "cq", cfg.cq, 0);
            c->bit_rate = 0;
        } else {
            av_opt_set(c->priv_data, "rc", "vbr", 0);
            c->bit_rate = cfg.bitrate;
            c->rc_max_rate = cfg.maxBitrate;
            c->rc_buffer_size = int(cfg.maxBitrate * 2);
        }
        if (cfg.lookahead > 0 && cfg.gop != 1) av_opt_set_int(c->priv_data, "rc-lookahead", cfg.lookahead, 0);
        if (cfg.gop == 1) av_opt_set_int(c->priv_data, "forced-idr", 1, 0);
        av_opt_set_int(c->priv_data, "b_ref_mode", 0, 0);
    } else {
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
        if (cfg.cq > 0) av_opt_set_int(c->priv_data, "crf", cfg.cq, 0);
        else {
            c->bit_rate = cfg.bitrate;
            c->rc_max_rate = cfg.maxBitrate;
            c->rc_buffer_size = int(cfg.maxBitrate * 2);
        }
        if (cfg.gop == 1) av_opt_set(c->priv_data, "x264-params", "keyint=1", 0);
    }
    return true;
}

bool VideoEncoder::open(const VideoEncodeConfig& cfg, std::shared_ptr<GpuDevice> gpu, std::string& error) {
    ff::initLogging();
    auto d = std::make_unique<Impl>();
    d->cfg = cfg;
    d->gpu = gpu;
    const AVCodec* codec = avcodec_find_encoder_by_name(cfg.codec.c_str());
    if (!codec) {
        error = "Encoder " + cfg.codec + " not available";
        return false;
    }
    const bool nvenc = cfg.codec.find("nvenc") != std::string::npos;
    d->name = cfg.codec;
    // 1) zero-copy D3D11 frames
    if (nvenc && cfg.zeroCopy && gpu) {
        d->cc.reset(avcodec_alloc_context3(codec));
        configureCommon(d->cc.get(), cfg, true);
        AVBufferRef* dev = ffHwDeviceFor(*gpu);
        AVBufferRef* fr = dev ? av_hwframe_ctx_alloc(dev) : nullptr;
        if (fr) {
            auto* fc = reinterpret_cast<AVHWFramesContext*>(fr->data);
            fc->format = AV_PIX_FMT_D3D11;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = cfg.width;
            fc->height = cfg.height;
            fc->initial_pool_size = 0;  // individual textures (NVENC registers each)
            auto* dfc = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
            dfc->BindFlags = D3D11_BIND_RENDER_TARGET;
            if (av_hwframe_ctx_init(fr) >= 0) {
                d->cc->pix_fmt = AV_PIX_FMT_D3D11;
                d->cc->hw_frames_ctx = av_buffer_ref(fr);
                if (avcodec_open2(d->cc.get(), codec, nullptr) >= 0) {
                    d->frames.reset(fr);
                    zeroCopy_ = true;
                    hardware_ = true;
                    d_ = std::move(d);
                    return true;
                }
            }
            av_buffer_unref(&fr);
        }
        d->cc.reset();
    }
    // 2/3) system memory NV12
    d->cc.reset(avcodec_alloc_context3(codec));
    configureCommon(d->cc.get(), cfg, nvenc);
    d->cc->pix_fmt = AV_PIX_FMT_NV12;
    if (!nvenc) d->cc->thread_count = 0;
    int r = avcodec_open2(d->cc.get(), codec, nullptr);
    if (r < 0) {
        error = "Cannot open encoder " + cfg.codec + ": " + ff::err(r);
        return false;
    }
    d->swFrame = ff::makeFrame();
    d->swFrame->format = AV_PIX_FMT_NV12;
    d->swFrame->width = cfg.width;
    d->swFrame->height = cfg.height;
    if (av_frame_get_buffer(d->swFrame.get(), 64) < 0) {
        error = "Out of memory";
        return false;
    }
    hardware_ = nvenc;
    zeroCopy_ = false;
    d_ = std::move(d);
    return true;
}

bool VideoEncoder::drain() {
    for (;;) {
        int r = avcodec_receive_packet(d_->cc.get(), d_->pkt.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
        if (r < 0) return false;
        bool ok = sink_ ? sink_(d_->pkt.get()) : true;
        av_packet_unref(d_->pkt.get());
        if (!ok) return false;
    }
}

bool VideoEncoder::encodeTexture(const GpuTexture& nv12, int64_t idx) {
    if (!d_ || !zeroCopy_) return false;
    ff::FramePtr f = ff::makeFrame();
    if (av_hwframe_get_buffer(d_->frames.get(), f.get(), 0) < 0) return false;
    auto* dst = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    UINT slice = UINT(reinterpret_cast<intptr_t>(f->data[1]));
    D3D11_BOX box{0, 0, 0, UINT(d_->cfg.width), UINT(d_->cfg.height), 1};
    d_->gpu->context()->CopySubresourceRegion(dst, slice, 0, 0, 0, nv12.tex.Get(), 0, &box);
    f->pts = idx;
    f->color_range = AVCOL_RANGE_MPEG;
    int r = avcodec_send_frame(d_->cc.get(), f.get());
    if (r < 0) return false;
    ++sent_;
    return drain();
}

bool VideoEncoder::encodeNV12(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int64_t idx) {
    if (!d_ || zeroCopy_) return false;
    AVFrame* f = d_->swFrame.get();
    if (av_frame_make_writable(f) < 0) return false;
    for (int row = 0; row < d_->cfg.height; ++row)
        std::memcpy(f->data[0] + size_t(row) * f->linesize[0], y + size_t(row) * yPitch, size_t(d_->cfg.width));
    for (int row = 0; row < d_->cfg.height / 2; ++row)
        std::memcpy(f->data[1] + size_t(row) * f->linesize[1], uv + size_t(row) * uvPitch, size_t(d_->cfg.width));
    f->pts = idx;
    f->color_range = AVCOL_RANGE_MPEG;
    int r = avcodec_send_frame(d_->cc.get(), f);
    if (r < 0) return false;
    ++sent_;
    return drain();
}

bool VideoEncoder::flush() {
    if (!d_) return false;
    avcodec_send_frame(d_->cc.get(), nullptr);
    return drain();
}

// ---------------------------------------------------------------------------------------------
struct AudioEncoder::Impl {
    ff::CodecPtr cc;
    ff::FramePtr frame;
    ff::PacketPtr pkt = ff::makePacket();
    std::vector<float> pending;  // interleaved
    int64_t pts = 0;
};

AudioEncoder::AudioEncoder() = default;
AudioEncoder::~AudioEncoder() = default;
AVCodecContext* AudioEncoder::context() const { return d_ ? d_->cc.get() : nullptr; }

bool AudioEncoder::open(int bitrate, std::string& error, const std::string& codecName) {
    auto d = std::make_unique<Impl>();
    const AVCodec* codec = codecName == "aac" ? avcodec_find_encoder(AV_CODEC_ID_AAC) : avcodec_find_encoder_by_name(codecName.c_str());
    if (!codec) {
        error = codecName + " encoder not available";
        return false;
    }
    // the first of these the encoder takes (float planar for AAC/MP3, 16-bit for FLAC/WAV)
    AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    {
        const void* cfg = nullptr;
        int n = 0;
        avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n);
        const auto* fmts = static_cast<const AVSampleFormat*>(cfg);
        for (AVSampleFormat want : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32})
            for (int i = 0; fmt == AV_SAMPLE_FMT_NONE && fmts && i < n; ++i)
                if (fmts[i] == want) fmt = want;
        if (fmt == AV_SAMPLE_FMT_NONE) fmt = fmts && n > 0 ? fmts[0] : AV_SAMPLE_FMT_FLTP;
    }
    d->cc.reset(avcodec_alloc_context3(codec));
    d->cc->sample_rate = 48000;
    d->cc->sample_fmt = fmt;
    av_channel_layout_default(&d->cc->ch_layout, 2);
    d->cc->bit_rate = bitrate;
    d->cc->time_base = AVRational{1, 48000};
    if (codec->id == AV_CODEC_ID_AAC) d->cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(d->cc.get(), codec, nullptr);
    if (r < 0) {
        error = "Cannot open " + codecName + " encoder: " + ff::err(r);
        return false;
    }
    d->frame = ff::makeFrame();
    d->frame->format = fmt;
    d->frame->sample_rate = 48000;
    av_channel_layout_default(&d->frame->ch_layout, 2);
    d->frame->nb_samples = d->cc->frame_size > 0 ? d->cc->frame_size : 1024;
    av_frame_get_buffer(d->frame.get(), 0);
    d_ = std::move(d);
    return true;
}

bool AudioEncoder::sendFrame(bool final) {
    Impl& d = *d_;
    const int n = d.frame->nb_samples;
    while (int(d.pending.size() / 2) >= n || (final && !d.pending.empty())) {
        int take = std::min<int>(n, int(d.pending.size() / 2));
        av_frame_make_writable(d.frame.get());
        auto sample = [&](int i, int ch) { return i < take ? d.pending[size_t(i) * 2 + size_t(ch)] : 0.f; };
        auto s16 = [](float v) { return int16_t(std::lrint(std::clamp(v, -1.f, 1.f) * 32767.f)); };
        auto s32 = [](float v) { return int32_t(std::llrint(std::clamp(double(v), -1.0, 1.0) * 2147483647.0)); };
        // last frame of a fixed-size codec is padded with silence; variable-size ones get just the rest
        const bool shortOk = d.cc->codec->capabilities & (AV_CODEC_CAP_VARIABLE_FRAME_SIZE | AV_CODEC_CAP_SMALL_LAST_FRAME);
        const int fill = take < n && shortOk ? take : n;
        d.frame->nb_samples = fill;
        for (int i = 0; i < fill; ++i)
            for (int ch = 0; ch < 2; ++ch) switch (d.cc->sample_fmt) {
                case AV_SAMPLE_FMT_FLTP: reinterpret_cast<float*>(d.frame->data[ch])[i] = sample(i, ch); break;
                case AV_SAMPLE_FMT_FLT: reinterpret_cast<float*>(d.frame->data[0])[i * 2 + ch] = sample(i, ch); break;
                case AV_SAMPLE_FMT_S16P: reinterpret_cast<int16_t*>(d.frame->data[ch])[i] = s16(sample(i, ch)); break;
                case AV_SAMPLE_FMT_S16: reinterpret_cast<int16_t*>(d.frame->data[0])[i * 2 + ch] = s16(sample(i, ch)); break;
                case AV_SAMPLE_FMT_S32P: reinterpret_cast<int32_t*>(d.frame->data[ch])[i] = s32(sample(i, ch)); break;
                default: reinterpret_cast<int32_t*>(d.frame->data[0])[i * 2 + ch] = s32(sample(i, ch)); break;
                }
        d.pending.erase(d.pending.begin(), d.pending.begin() + take * 2);
        d.frame->pts = d.pts;
        d.pts += fill;
        const int sent = avcodec_send_frame(d.cc.get(), d.frame.get());
        d.frame->nb_samples = n;  // the buffer keeps its full size for the next frame
        if (sent < 0) return false;
        for (;;) {
            int rr = avcodec_receive_packet(d.cc.get(), d.pkt.get());
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
            if (rr < 0) return false;
            bool ok = sink_ ? sink_(d.pkt.get()) : true;
            av_packet_unref(d.pkt.get());
            if (!ok) return false;
        }
    }
    return true;
}

bool AudioEncoder::encode(const float* in, int frames) {
    if (!d_) return false;
    d_->pending.insert(d_->pending.end(), in, in + size_t(frames) * 2);
    return sendFrame(false);
}

bool AudioEncoder::flush() {
    if (!d_) return false;
    if (!sendFrame(true)) return false;
    avcodec_send_frame(d_->cc.get(), nullptr);
    for (;;) {
        int rr = avcodec_receive_packet(d_->cc.get(), d_->pkt.get());
        if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
        if (rr < 0) return false;
        bool ok = sink_ ? sink_(d_->pkt.get()) : true;
        av_packet_unref(d_->pkt.get());
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
struct Muxer::Impl {
    AVFormatContext* oc = nullptr;
    std::string path;
    std::mutex m;
    bool headerWritten = false;
    bool faststart = false;
    ~Impl() {
        if (oc) {
            if (oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }
};

Muxer::Muxer() = default;
Muxer::~Muxer() = default;

bool Muxer::open(const std::string& path, std::string& error) {
    auto d = std::make_unique<Impl>();
    d->path = path;
    int r = avformat_alloc_output_context2(&d->oc, nullptr, nullptr, path.c_str());
    if (r < 0 || !d->oc) {
        error = "Unknown output container for " + path;
        return false;
    }
    r = avio_open(&d->oc->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) {
        error = "Cannot create output file: " + ff::err(r);
        return false;
    }
    d_ = std::move(d);
    return true;
}

int Muxer::addStream(AVCodecContext* enc, bool faststart) {
    AVStream* st = avformat_new_stream(d_->oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, enc);
    st->time_base = enc->time_base;
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO) st->avg_frame_rate = enc->framerate;
    d_->faststart = d_->faststart || faststart;
    return st->index;
}

bool Muxer::writeHeader(std::string& error) {
    AVDictionary* opts = nullptr;
    if (d_->faststart) av_dict_set(&opts, "movflags", "+faststart", 0);
    int r = avformat_write_header(d_->oc, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        error = "Cannot write header: " + ff::err(r);
        return false;
    }
    d_->headerWritten = true;
    return true;
}

bool Muxer::write(AVPacket* pkt, int stream, AVCodecContext* enc) {
    std::lock_guard<std::mutex> l(d_->m);
    AVStream* st = d_->oc->streams[stream];
    av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
    pkt->stream_index = stream;
    return av_interleaved_write_frame(d_->oc, pkt) >= 0;
}

bool Muxer::finish(std::string& error) {
    if (!d_) return false;
    std::lock_guard<std::mutex> l(d_->m);
    int r = av_write_trailer(d_->oc);
    avio_closep(&d_->oc->pb);
    if (r < 0) {
        error = "Cannot finalize file: " + ff::err(r);
        return false;
    }
    return true;
}

void Muxer::abort() {
    if (!d_) return;
    std::string p = d_->path;
    d_.reset();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(std::u8string(p.begin(), p.end())), ec);
}

bool encoderUsable(const std::string& name, std::shared_ptr<GpuDevice> gpu) {
    static std::mutex m;
    static std::map<std::string, bool> cache;
    std::lock_guard<std::mutex> l(m);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    VideoEncodeConfig c;
    c.codec = name;
    c.width = 256;
    c.height = 144;
    c.zeroCopy = false;
    c.lookahead = 0;
    VideoEncoder e;
    std::string err;
    bool ok = e.open(c, gpu, err);
    cache[name] = ok;
    return ok;
}

} // namespace cut
