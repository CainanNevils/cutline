#include "media/thumbnailer.h"

#include "media/ff.h"

#include <algorithm>

namespace cut {

bool extractThumbnails(const std::string& path, const MediaInfo& info, int thumbH, Tick interval,
                       const std::function<void(Tick, const uint8_t*, int, int, int)>& sink,
                       const std::function<bool()>& cancelled) {
    std::string err;
    ff::FormatPtr fmt = ff::openInput(path, err);
    if (!fmt || info.video.index < 0) return false;
    const int idx = info.video.index;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) fmt->streams[i]->discard = int(i) == idx ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    AVStream* st = fmt->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;
    ff::CodecPtr cc(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(cc.get(), st->codecpar);
    cc->pkt_timebase = st->time_base;
    cc->thread_count = 2;
    if (!info.isImage) cc->skip_frame = AVDISCARD_NONKEY;  // keyframes only: fast
    if (avcodec_open2(cc.get(), codec, nullptr) < 0) return false;
    const Rational tb = ff::rat(st->time_base);
    const int thumbW = std::max(2, int(double(thumbH) * info.video.width / std::max(1, info.video.height)) & ~1);
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr f = ff::makeFrame();
    ff::SwsPtr sws;
    std::vector<uint8_t> buf(size_t(thumbW) * thumbH * 4);
    Tick next = 0;
    bool eof = false;
    while (!eof) {
        if (cancelled && cancelled()) return false;
        int r = av_read_frame(fmt.get(), pkt.get());
        if (r < 0) {
            avcodec_send_packet(cc.get(), nullptr);
            eof = true;
        } else {
            if (pkt->stream_index == idx) avcodec_send_packet(cc.get(), pkt.get());
            av_packet_unref(pkt.get());
        }
        while (avcodec_receive_frame(cc.get(), f.get()) == 0) {
            int64_t p = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            Tick t = p == AV_NOPTS_VALUE ? 0 : streamToTicks(p, tb) - info.origin;
            if (t + interval / 2 >= next || info.isImage) {
                if (!sws)
                    sws.reset(sws_getContext(f->width, f->height, AVPixelFormat(f->format), thumbW, thumbH, AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr));
                if (sws) {
                    uint8_t* d[4] = {buf.data(), nullptr, nullptr, nullptr};
                    int ls[4] = {thumbW * 4, 0, 0, 0};
                    sws_scale(sws.get(), f->data, f->linesize, 0, f->height, d, ls);
                    sink(std::max<Tick>(0, t), buf.data(), thumbW, thumbH, thumbW * 4);
                }
                next = std::max(next, t) + interval;
            }
            av_frame_unref(f.get());
            if (info.isImage) return true;
        }
    }
    return true;
}

} // namespace cut
