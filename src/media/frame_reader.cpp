#include "media/frame_reader.h"

#include "media/ff.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace cut {

bool FrameReader::open(const std::string& path, std::shared_ptr<GpuDevice> gpu, const MediaInfo& info,
                       const VideoDecoder::Options& opt, std::string& error) {
    image_ = info.isImage;
    mediaDur_ = info.duration;
    if (!dec_.open(path, std::move(gpu), info.origin, opt, error)) return false;
    frameDur_ = info.video.fps.valid() ? cut::frameDuration(info.video.fps) : dec_.frameDurationHint();
    tol_ = frameDur_ / 2;
    if (image_) {
        DecodedPicture p;
        if (!dec_.next(INT64_MIN, p) || !p.frame) {
            error = "Could not decode image";
            return false;
        }
        still_ = p.frame;
        still_->pts = 0;
        still_->end = INT64_MAX / 4;
        dec_.close();
    }
    return true;
}

void FrameReader::clearWindow() {
    std::lock_guard<std::mutex> l(mutex_);
    win_.clear();
    lastKept_.reset();
}

std::vector<GpuFramePtr> FrameReader::frames() const {
    std::lock_guard<std::mutex> l(mutex_);
    return {win_.begin(), win_.end()};
}

GpuFramePtr FrameReader::findLocked(Tick t) const {
    if (still_) return still_;
    if (win_.empty()) return nullptr;
    Tick q = t + tol_;
    auto it = std::upper_bound(win_.begin(), win_.end(), q, [](Tick v, const GpuFramePtr& f) { return v < f->pts; });
    if (it == win_.begin()) return nullptr;
    const GpuFramePtr& f = *(it - 1);
    if (q < f->end.load() || (mediaDur_ > 0 && t >= mediaDur_ - frameDur_ && it == win_.end() && dec_.atEof()))
        return f;
    return nullptr;
}

GpuFramePtr FrameReader::peek(Tick t) const {
    std::lock_guard<std::mutex> l(mutex_);
    return findLocked(t);
}

GpuFramePtr FrameReader::nextAfter(const GpuFramePtr& f) const {
    if (!f || still_) return nullptr;
    std::lock_guard<std::mutex> l(mutex_);
    auto it = std::upper_bound(win_.begin(), win_.end(), f->pts, [](Tick v, const GpuFramePtr& x) { return v < x->pts; });
    if (it == win_.end()) return nullptr;
    if ((*it)->pts != f->end.load()) return nullptr;  // not consecutive
    return *it;
}

void FrameReader::insert(const GpuFramePtr& f, Tick focus) {
    std::lock_guard<std::mutex> l(mutex_);
    if (lastKept_ && lastKept_->pts < f->pts) lastKept_->end = f->pts;
    lastKept_ = f;
    auto it = std::lower_bound(win_.begin(), win_.end(), f->pts, [](const GpuFramePtr& x, Tick v) { return x->pts < v; });
    if (it != win_.end() && (*it)->pts == f->pts) *it = f;
    else win_.insert(it, f);
    while (int(win_.size()) > window_) {
        // drop whichever end is farther from the focus
        if (std::llabs(win_.front()->pts - focus) >= std::llabs(win_.back()->pts - focus)) win_.pop_front();
        else win_.pop_back();
    }
    ++stats_.kept;
}

bool FrameReader::seekTo(Tick target) {
    ++stats_.seeks;
    {
        std::lock_guard<std::mutex> l(mutex_);
        lastKept_.reset();  // the next decoded frame is not the successor of the last kept one
    }
    lastPts_ = kInvalidTick;
    return dec_.seek(target);
}

GpuFramePtr FrameReader::get(Tick t, const std::function<bool()>& cancel) {
    if (still_) return still_;
    if (!dec_.isOpen()) return nullptr;
    if (mediaDur_ > 0) t = std::clamp<Tick>(t, 0, std::max<Tick>(0, mediaDur_ - 1));
    if (GpuFramePtr f = peek(t)) return f;

    auto t0 = std::chrono::steady_clock::now();
    const Tick threshold = std::max<Tick>(3 * kTicksPerSecond, frameDur_ * 4);
    bool forward = lastPts_ != kInvalidTick && t > lastPts_ && t - lastPts_ <= threshold && !dec_.atEof();
    Tick keepFrom = t - Tick(window_ - 2) * frameDur_;
    bool seeked = false;
    Tick seekTarget = t;
    if (!forward) {
        Tick front = kInvalidTick;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (!win_.empty()) front = win_.front()->pts;
        }
        if (front != kInvalidTick && t < front && front - t <= frameDur_ * 4) {
            // stepping backwards just below the window: refill a window ending at t
            seekTarget = std::max<Tick>(0, t - Tick(window_ - 4) * frameDur_);
            keepFrom = seekTarget;
        } else {
            keepFrom = t - std::min<Tick>(Tick(window_ / 4), 8) * frameDur_;
        }
        seekTo(seekTarget);
        seeked = true;
    }
    static const Tick backoff[] = {2 * kTicksPerSecond, 5 * kTicksPerSecond, 15 * kTicksPerSecond};
    int retries = 0;
    bool first = true;
    GpuFramePtr lastFrame;
    for (;;) {
        if (cancel && cancel()) return nullptr;
        DecodedPicture p;
        if (!dec_.next(keepFrom, p)) {
            // end of stream: the last frame covers everything after it
            std::lock_guard<std::mutex> l(mutex_);
            if (!win_.empty()) {
                win_.back()->end = INT64_MAX / 4;
                auto f = findLocked(t);
                return f ? f : win_.back();
            }
            return lastFrame;
        }
        ++stats_.decoded;
        lastPts_ = p.pts;
        if (first && seeked && p.pts > seekTarget + tol_ && seekTarget > 0 && retries < 3) {
            // demuxer landed after the target (sparse index): seek further back
            Tick back = std::max<Tick>(0, seekTarget - backoff[retries++]);
            seekTo(back);
            continue;
        }
        first = false;
        if (p.frame) {
            insert(p.frame, t);
            lastFrame = p.frame;
        }
        if (p.pts > t + tol_) {
            if (seeked) {
                auto t1 = std::chrono::steady_clock::now();
                stats_.lastSeekMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            }
            GpuFramePtr f = peek(t);
            return f ? f : lastFrame;  // t before the first frame: show the first frame
        }
    }
}

// -------------------------------------------------------------------------------------------
// AudioReader
struct AudioReader::Impl {
    ff::FormatPtr fmt;
    ff::CodecPtr cc;
    ff::SwrPtr swr;
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr frame = ff::makeFrame();
    int stream = -1;
    Rational tb{1, 1};
    Tick origin = 0;
    std::vector<float> fifo;  // interleaved stereo
    int64_t fifoStart = 0;    // sample position of fifo[0]
    bool eof = false;
    bool valid = false;  // fifoStart meaningful
    int inRate = 0, inFmt = -1;
    AVChannelLayout inLayout{};

    ~Impl() { av_channel_layout_uninit(&inLayout); }

    bool setupSwr(const AVFrame* f) {
        if (swr && inRate == f->sample_rate && inFmt == f->format && !av_channel_layout_compare(&inLayout, &f->ch_layout))
            return true;
        SwrContext* s = nullptr;
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout in{};
        av_channel_layout_copy(&in, &f->ch_layout);
        if (in.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, f->ch_layout.nb_channels);
        if (swr_alloc_set_opts2(&s, &out, AV_SAMPLE_FMT_FLT, 48000, &in, AVSampleFormat(f->format), f->sample_rate, 0,
                                nullptr) < 0) {
            av_channel_layout_uninit(&in);
            return false;
        }
        // Mono -> centred stereo at equal level (not -3 dB), >2 ch -> standard downmix.
        if (in.nb_channels == 1) av_opt_set_double(s, "center_mix_level", 1.0, 0);
        if (swr_init(s) < 0) {
            swr_free(&s);
            av_channel_layout_uninit(&in);
            return false;
        }
        swr.reset(s);
        inRate = f->sample_rate;
        inFmt = f->format;
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_copy(&inLayout, &in);
        av_channel_layout_uninit(&in);
        return true;
    }

    // Decodes one frame and appends to the fifo. Returns false at EOF.
    bool decodeMore() {
        AVFrame* f = frame.get();
        for (;;) {
            int r = avcodec_receive_frame(cc.get(), f);
            if (r == 0) break;
            if (r == AVERROR_EOF || eof) return false;
            int rr = av_read_frame(fmt.get(), pkt.get());
            if (rr < 0) {
                avcodec_send_packet(cc.get(), nullptr);
                eof = true;
                continue;
            }
            if (pkt->stream_index == stream) avcodec_send_packet(cc.get(), pkt.get());
            av_packet_unref(pkt.get());
        }
        if (!setupSwr(f)) {
            av_frame_unref(f);
            return true;
        }
        if (!valid) {
            int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            Tick t = pts == AV_NOPTS_VALUE ? 0 : streamToTicks(pts, tb) - origin;
            fifoStart = ticksToSamples(t, 48000) - int64_t(fifo.size() / 2);
            valid = true;
        }
        int outMax = swr_get_out_samples(swr.get(), f->nb_samples);
        size_t old = fifo.size();
        fifo.resize(old + size_t(std::max(0, outMax)) * 2);
        uint8_t* outp = reinterpret_cast<uint8_t*>(fifo.data() + old);
        int got = swr_convert(swr.get(), &outp, outMax, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        fifo.resize(old + size_t(std::max(0, got)) * 2);
        av_frame_unref(f);
        return true;
    }

    void seek(int64_t pos) {
        Tick t = samplesToTicks(std::max<int64_t>(0, pos - 4800), 48000);  // 100 ms preroll
        int64_t ts = ticksToStream(t + origin, tb);
        av_seek_frame(fmt.get(), stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cc.get());
        if (swr) swr_init(swr.get());
        fifo.clear();
        valid = false;
        eof = false;
    }
};

AudioReader::AudioReader() = default;
AudioReader::~AudioReader() = default;
bool AudioReader::isOpen() const { return d_ && d_->cc; }

bool AudioReader::open(const std::string& path, int streamIndex, Tick origin, std::string& error) {
    auto d = std::make_unique<Impl>();
    d->fmt = ff::openInput(path, error);
    if (!d->fmt) return false;
    if (streamIndex < 0 || unsigned(streamIndex) >= d->fmt->nb_streams ||
        d->fmt->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        error = "Audio stream not found";
        return false;
    }
    for (unsigned i = 0; i < d->fmt->nb_streams; ++i)
        d->fmt->streams[i]->discard = int(i) == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    AVStream* st = d->fmt->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        error = "Unsupported audio codec";
        return false;
    }
    d->cc.reset(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(d->cc.get(), st->codecpar);
    d->cc->pkt_timebase = st->time_base;
    if (avcodec_open2(d->cc.get(), codec, nullptr) < 0) {
        error = "Cannot open audio decoder";
        return false;
    }
    d->stream = streamIndex;
    d->tb = ff::rat(st->time_base);
    d->origin = origin;
    Tick dur = st->duration > 0 ? streamToTicks(st->duration, d->tb)
                                : (d->fmt->duration > 0 ? streamToTicks(d->fmt->duration, {1, AV_TIME_BASE}) : 0);
    Tick start = st->start_time != AV_NOPTS_VALUE ? streamToTicks(st->start_time, d->tb) - origin : 0;
    length_ = ticksToSamples(start + dur, 48000);
    d_ = std::move(d);
    return true;
}

void AudioReader::read(int64_t pos, float* out, int frames) {
    std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    if (!d_ || frames <= 0) return;
    Impl& d = *d_;
    if (pos + frames <= 0 || (length_ > 0 && pos >= length_ + 48000)) return;
    int64_t fifoEnd = d.fifoStart + int64_t(d.fifo.size() / 2);
    // Non-contiguous request (before the buffer or far ahead) -> seek.
    if (!d.valid || pos < d.fifoStart || pos > fifoEnd + 48000 / 2) {
        d.seek(pos);
    }
    // Decode until the buffer covers [pos, pos+frames).
    while (true) {
        fifoEnd = d.valid ? d.fifoStart + int64_t(d.fifo.size() / 2) : INT64_MIN;
        if (d.valid && fifoEnd >= pos + frames) break;
        if (!d.decodeMore()) break;
    }
    if (!d.valid) return;
    // copy overlap
    int64_t a = std::max(pos, d.fifoStart);
    int64_t b = std::min(pos + frames, d.fifoStart + int64_t(d.fifo.size() / 2));
    if (b > a)
        std::memcpy(out + (a - pos) * 2, d.fifo.data() + (a - d.fifoStart) * 2, size_t(b - a) * 2 * sizeof(float));
    // drop consumed samples, keep 0.5 s of history for small backward jitter
    int64_t keepFrom = pos + frames - 24000;
    if (keepFrom > d.fifoStart) {
        int64_t drop = std::min<int64_t>(keepFrom - d.fifoStart, int64_t(d.fifo.size() / 2));
        d.fifo.erase(d.fifo.begin(), d.fifo.begin() + drop * 2);
        d.fifoStart += drop;
    }
}

} // namespace cut
