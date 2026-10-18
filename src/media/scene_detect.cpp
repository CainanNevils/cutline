#include "media/scene_detect.h"

#include "media/ff.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace cut {

std::vector<size_t> pickSceneCuts(const std::vector<double>& scores, const std::vector<Tick>& times, const SceneOptions& opt) {
    std::vector<size_t> out;
    Tick last = INT64_MIN / 2;
    for (size_t i = 1; i < scores.size(); ++i) {
        // recent motion level: mean of the previous (up to) 12 scores
        double sum = 0;
        int n = 0;
        for (size_t k = i > 12 ? i - 12 : 1; k < i; ++k, ++n) sum += scores[k];
        const double recent = n ? sum / n : 0;
        const bool peak = scores[i] >= opt.threshold && scores[i] > 3.0 * recent &&
                          (i + 1 >= scores.size() || scores[i] >= scores[i + 1]);
        if (!peak || times[i] - last < opt.minGap) continue;
        out.push_back(i);
        last = times[i];
    }
    return out;
}

bool detectScenes(const std::string& path, const MediaInfo& info, Tick from, Tick to, const SceneOptions& opt,
                  std::vector<Tick>& cuts, const std::function<void(double)>& progress, const std::function<bool()>& cancelled) {
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
    cc->thread_count = 0;
    cc->skip_frame = AVDISCARD_NONREF;  // plenty of samples, much less decoding
    if (avcodec_open2(cc.get(), codec, nullptr) < 0) return false;
    const Rational tb = ff::rat(st->time_base);
    if (from > 0) {
        const int64_t ts = ticksToStream(from + info.origin, tb);
        av_seek_frame(fmt.get(), idx, ts, AVSEEK_FLAG_BACKWARD);
    }
    constexpr int W = 32, H = 18;
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr f = ff::makeFrame();
    ff::SwsPtr sws;
    std::array<uint8_t, W * H> cur{}, prev{};
    std::vector<double> scores;
    std::vector<Tick> times;
    bool have = false, eof = false, done = false;
    int64_t lastReport = 0;
    while (!eof && !done) {
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
            const int64_t p = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            const Tick t = p == AV_NOPTS_VALUE ? 0 : streamToTicks(p, tb) - info.origin;
            if (t < from) {
                av_frame_unref(f.get());
                continue;
            }
            if (t >= to) {
                done = true;
                av_frame_unref(f.get());
                break;
            }
            if (!sws)
                sws.reset(sws_getContext(f->width, f->height, AVPixelFormat(f->format), W, H, AV_PIX_FMT_GRAY8, SWS_AREA,
                                         nullptr, nullptr, nullptr));
            if (sws) {
                uint8_t* d[4] = {cur.data(), nullptr, nullptr, nullptr};
                int ls[4] = {W, 0, 0, 0};
                sws_scale(sws.get(), f->data, f->linesize, 0, f->height, d, ls);
                double score = 0;
                if (have) {
                    // mean absolute pixel difference + luma histogram difference (16 bins)
                    int mad = 0;
                    std::array<int, 16> ha{}, hb{};
                    for (int i = 0; i < W * H; ++i) {
                        mad += std::abs(int(cur[size_t(i)]) - int(prev[size_t(i)]));
                        ++ha[cur[size_t(i)] >> 4];
                        ++hb[prev[size_t(i)] >> 4];
                    }
                    int hd = 0;
                    for (int b = 0; b < 16; ++b) hd += std::abs(ha[size_t(b)] - hb[size_t(b)]);
                    score = std::max(2.0 * mad / (255.0 * W * H), hd / (2.0 * W * H));
                }
                scores.push_back(score);
                times.push_back(t);
                prev = cur;
                have = true;
            }
            av_frame_unref(f.get());
            if (progress && to > from && t - lastReport > kTicksPerSecond * 5) {
                lastReport = t;
                progress(std::clamp(double(t - from) / double(to - from), 0.0, 1.0));
            }
        }
    }
    cuts.clear();
    for (size_t i : pickSceneCuts(scores, times, opt)) cuts.push_back(times[i]);
    return true;
}

} // namespace cut
