#include "audio/clip_audio.h"

#include "media/ff.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace cut {

// ---------------------------------------------------------------------------------------------
struct TempoStretcher::Impl {
    ff::GraphPtr graph;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    ff::FramePtr in = ff::makeFrame();
    ff::FramePtr out = ff::makeFrame();
    std::vector<float> pending;  // produced but not yet pulled
    int64_t pts = 0;
};

TempoStretcher::TempoStretcher(double speed) : d_(std::make_unique<Impl>()) {
    // atempo accepts 0.5..2.0 per instance (100 in newer builds, but chained is higher quality)
    std::string chain;
    double s = speed;
    while (s > 2.0 + 1e-9) {
        chain += "atempo=2.0,";
        s /= 2.0;
    }
    while (s < 0.5 - 1e-9) {
        chain += "atempo=0.5,";
        s /= 0.5;
    }
    chain += "atempo=" + std::to_string(s);
    AVFilterGraph* g = avfilter_graph_alloc();
    d_->graph.reset(g);
    const AVFilter* abuf = avfilter_get_by_name("abuffer");
    const AVFilter* asink = avfilter_get_by_name("abuffersink");
    char args[256];
    std::snprintf(args, sizeof(args), "time_base=1/48000:sample_rate=48000:sample_fmt=flt:channel_layout=stereo");
    if (avfilter_graph_create_filter(&d_->src, abuf, "in", args, nullptr, g) < 0) return;
    d_->sink = avfilter_graph_alloc_filter(g, asink, "out");
    if (!d_->sink) return;
    static const int fmts[] = {AV_SAMPLE_FMT_FLT};
    av_opt_set_array(d_->sink, "sample_formats", AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0, 1, AV_OPT_TYPE_INT,
                     fmts);
    if (avfilter_init_dict(d_->sink, nullptr) < 0) {
        d_->sink = nullptr;
        return;
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = d_->src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = d_->sink;
    int r = avfilter_graph_parse_ptr(g, chain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (r < 0 || avfilter_graph_config(g, nullptr) < 0) d_->sink = nullptr;
}

TempoStretcher::~TempoStretcher() = default;
bool TempoStretcher::ok() const { return d_->sink != nullptr; }

void TempoStretcher::push(const float* in, int frames) {
    if (!ok() || frames <= 0) return;
    AVFrame* f = d_->in.get();
    av_frame_unref(f);
    f->format = AV_SAMPLE_FMT_FLT;
    f->sample_rate = 48000;
    av_channel_layout_default(&f->ch_layout, 2);
    f->nb_samples = frames;
    if (av_frame_get_buffer(f, 0) < 0) return;
    std::memcpy(f->data[0], in, size_t(frames) * 8);
    f->pts = d_->pts;
    d_->pts += frames;
    (void)av_buffersrc_add_frame_flags(d_->src, f, AV_BUFFERSRC_FLAG_KEEP_REF);
    for (;;) {
        AVFrame* o = d_->out.get();
        if (av_buffersink_get_frame(d_->sink, o) < 0) break;
        const float* p = reinterpret_cast<const float*>(o->data[0]);
        d_->pending.insert(d_->pending.end(), p, p + size_t(o->nb_samples) * 2);
        av_frame_unref(o);
    }
}

void TempoStretcher::flushInput() {
    if (!ok()) return;
    (void)av_buffersrc_add_frame(d_->src, nullptr);
    for (;;) {
        AVFrame* o = d_->out.get();
        if (av_buffersink_get_frame(d_->sink, o) < 0) break;
        const float* p = reinterpret_cast<const float*>(o->data[0]);
        d_->pending.insert(d_->pending.end(), p, p + size_t(o->nb_samples) * 2);
        av_frame_unref(o);
    }
}

int TempoStretcher::pull(float* out, int n) {
    int got = std::min<int>(n, int(d_->pending.size() / 2));
    if (got > 0) {
        std::memcpy(out, d_->pending.data(), size_t(got) * 8);
        d_->pending.erase(d_->pending.begin(), d_->pending.begin() + size_t(got) * 2);
    }
    return got;
}

// ---------------------------------------------------------------------------------------------
ClipAudio::ClipAudio() = default;
ClipAudio::~ClipAudio() = default;

bool ClipAudio::open(const std::string& path, int streamIndex, Tick origin, std::string& error) {
    return reader_.open(path, streamIndex, origin, error);
}

void ClipAudio::renderForward(int64_t srcSample, float* out, int n) { reader_.read(srcSample, out, n); }

void ClipAudio::renderReverse(int64_t end, float* out, int n) {
    // out[i] = source sample (end - 1 - i)
    int64_t lo = end - n;
    if (lo < chunkStart_ || end > chunkEnd_) {
        chunkEnd_ = end;
        chunkStart_ = end - 48000;
        chunk_.resize(size_t(chunkEnd_ - chunkStart_) * 2);
        reader_.read(chunkStart_, chunk_.data(), int(chunkEnd_ - chunkStart_));
    }
    for (int i = 0; i < n; ++i) {
        int64_t s = end - 1 - i;
        if (s < chunkStart_ || s >= chunkEnd_) {
            out[i * 2] = out[i * 2 + 1] = 0;
            continue;
        }
        size_t k = size_t(s - chunkStart_) * 2;
        out[i * 2] = chunk_[k];
        out[i * 2 + 1] = chunk_[k + 1];
    }
}

void ClipAudio::render(const Clip& c, int64_t ts, float* out, int n) {
    std::memset(out, 0, size_t(n) * 8);
    if (c.isHold() || n <= 0) return;
    const int64_t startS = ticksToSamples(c.start, 48000);
    const int64_t inS = ticksToSamples(c.sourceIn, 48000);
    const int64_t outS = ticksToSamples(c.sourceOut(), 48000);
    const int64_t local = ts - startS;
    if (c.speed == 1.0) {
        if (!c.reverse) renderForward(inS + local, out, n);
        else renderReverse(outS - local, out, n);
        return;
    }
    // time-stretched
    if (!tempo_ || tempoSpeed_ != c.speed || tempoReverse_ != c.reverse || ts != expectedTs_) {
        tempo_ = std::make_unique<TempoStretcher>(c.speed);
        tempoSpeed_ = c.speed;
        tempoReverse_ = c.reverse;
        int64_t srcOff = int64_t(std::llround(double(local) * c.speed));
        feedPos_ = c.reverse ? outS - srcOff : inS + srcOff;
        if (!tempo_->ok()) {
            tempo_.reset();
            return;
        }
    }
    int produced = 0;
    int guard = 0;
    tmp_.resize(2048 * 2);
    while (produced < n && guard++ < 64) {
        produced += tempo_->pull(out + size_t(produced) * 2, n - produced);
        if (produced >= n) break;
        const int feed = 1024;
        if (!c.reverse) {
            renderForward(feedPos_, tmp_.data(), feed);
            feedPos_ += feed;
        } else {
            renderReverse(feedPos_, tmp_.data(), feed);
            feedPos_ -= feed;
        }
        tempo_->push(tmp_.data(), feed);
    }
    expectedTs_ = ts + n;
}

} // namespace cut
