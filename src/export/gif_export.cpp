#include "export/gif_export.h"

#include "engine/frame_renderer.h"
#include "engine/sync_provider.h"
#include "media/ff.h"

#include <algorithm>
#include <cstdio>

namespace cut {

bool exportGif(SnapshotPtr snapIn, const GifSettings& s, std::atomic<double>& progress, std::atomic<bool>& cancel,
               std::string& error) {
    if (!snapIn || s.to <= s.from) {
        error = "Nothing to export.";
        return false;
    }
    if (s.to - s.from > kGifMaxLength) {
        error = "GIFs are limited to 60 seconds - set In and Out around the moment.";
        return false;
    }
    RenderSnapshot snap = *snapIn;
    snap.useProxies = false;
    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
    const int w = std::max(16, s.width) & ~1;
    const int h = std::max(16, int(std::lround(double(w) * H / std::max(1, W)))) & ~1;
    const int fps = std::clamp(s.fps, 1, 50);

    auto gpu = GpuDevice::create(false, error);
    if (!gpu) return false;
    Compositor comp;
    if (!comp.init(gpu, error)) return false;
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, true);
    auto canvas = comp.makeCanvas(w, h, false);
    if (!canvas) {
        error = "Out of GPU memory.";
        return false;
    }

    // filter graph: rgba frames -> one palette for the whole clip -> paletted frames
    ff::GraphPtr graph(avfilter_graph_alloc());
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    char args[256];
    std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1", w, h, int(AV_PIX_FMT_RGBA), fps);
    if (avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args, nullptr, graph.get()) < 0 ||
        avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get()) < 0) {
        error = "Cannot set up the GIF palette filters.";
        return false;
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink;
    const char* chain = "split[a][b];[a]palettegen=max_colors=256:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle";
    int r = avfilter_graph_parse_ptr(graph.get(), chain, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (r < 0 || avfilter_graph_config(graph.get(), nullptr) < 0) {
        error = "Cannot set up the GIF palette filters.";
        return false;
    }

    // encoder + muxer
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_GIF);
    AVFormatContext* oc = nullptr;
    if (!codec || avformat_alloc_output_context2(&oc, nullptr, "gif", s.path.c_str()) < 0 || !oc) {
        error = "GIF encoder not available.";
        return false;
    }
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> out(oc, [](AVFormatContext* c) {
        if (c->pb) avio_closep(&c->pb);
        avformat_free_context(c);
    });
    ff::CodecPtr enc(avcodec_alloc_context3(codec));
    enc->width = w;
    enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_PAL8;
    enc->time_base = {1, fps};
    if (avcodec_open2(enc.get(), codec, nullptr) < 0) {
        error = "GIF encoder failed to open.";
        return false;
    }
    AVStream* st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, enc.get());
    st->time_base = enc->time_base;
    if (avio_open(&oc->pb, s.path.c_str(), AVIO_FLAG_WRITE) < 0) {
        error = "Cannot write " + s.path;
        return false;
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "loop", "0", 0);  // loop forever
    r = avformat_write_header(oc, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        error = "Cannot write the GIF header.";
        return false;
    }
    ff::PacketPtr pkt = ff::makePacket();
    auto drain = [&]() -> bool {
        while (avcodec_receive_packet(enc.get(), pkt.get()) == 0) {
            av_packet_rescale_ts(pkt.get(), enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (av_interleaved_write_frame(oc, pkt.get()) < 0) return false;
        }
        return true;
    };
    ff::FramePtr pf = ff::makeFrame();
    auto pull = [&]() -> bool {
        while (av_buffersink_get_frame(sink, pf.get()) >= 0) {
            pf->pict_type = AV_PICTURE_TYPE_NONE;
            if (avcodec_send_frame(enc.get(), pf.get()) < 0) return false;
            av_frame_unref(pf.get());
            if (!drain()) return false;
        }
        return true;
    };

    // render every frame of the range into the graph
    const Rational rate{fps, 1};
    const int64_t f0 = ticksToFrame(s.from, rate), f1 = ticksToFrame(s.to, rate);
    std::vector<uint8_t> rb;
    ff::FramePtr in = ff::makeFrame();
    for (int64_t f = f0; f < f1; ++f) {
        if (cancel.load()) {
            error = "Cancelled.";
            return false;
        }
        const Tick t = frameToTicks(f, rate);
        renderer.render(snap, t, *canvas, provider, true);
        int pitch = 0;
        if (!comp.readback(*canvas, rb, pitch)) {
            error = "GPU read-back failed.";
            return false;
        }
        av_frame_unref(in.get());
        in->format = AV_PIX_FMT_RGBA;
        in->width = w;
        in->height = h;
        in->pts = f - f0;
        if (av_frame_get_buffer(in.get(), 0) < 0) {
            error = "Out of memory.";
            return false;
        }
        for (int y = 0; y < h; ++y) {
            uint8_t* dst = in->data[0] + size_t(y) * in->linesize[0];
            const uint8_t* srow = rb.data() + size_t(y) * pitch;
            for (int x = 0; x < w; ++x) {  // premultiplied over black = the colour; GIF has no alpha here
                dst[x * 4 + 0] = srow[x * 4 + 0];
                dst[x * 4 + 1] = srow[x * 4 + 1];
                dst[x * 4 + 2] = srow[x * 4 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        if (av_buffersrc_add_frame(src, in.get()) < 0) {
            error = "GIF filter error.";
            return false;
        }
        progress = 0.8 * double(f - f0 + 1) / double(std::max<int64_t>(1, f1 - f0));
    }
    // the palette exists once every frame was seen: now the paletted frames come out
    if (av_buffersrc_add_frame(src, nullptr) < 0 || !pull()) {
        error = "GIF encoding failed.";
        return false;
    }
    avcodec_send_frame(enc.get(), nullptr);
    drain();
    av_write_trailer(oc);
    progress = 1.0;
    return true;
}

} // namespace cut
