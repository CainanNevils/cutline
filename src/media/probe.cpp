#include "media/probe.h"

#include "core/project_io.h"
#include "media/ff.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace cut {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
std::string ext(const std::string& p) {
    size_t d = p.rfind('.');
    return d == std::string::npos ? std::string() : lower(p.substr(d + 1));
}
const char* kImage[] = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp", "gif", "tga"};
const char* kVideo[] = {"mp4", "mkv", "mov", "m4v", "avi", "webm", "ts", "mts", "m2ts", "flv", "wmv", "mpg", "mpeg"};
const char* kAudio[] = {"wav", "mp3", "flac", "aac", "m4a", "ogg", "opus", "wma", "aiff", "aif"};

ColorRange mapRange(AVColorRange r) {
    if (r == AVCOL_RANGE_JPEG) return ColorRange::Full;
    if (r == AVCOL_RANGE_MPEG) return ColorRange::Limited;
    return ColorRange::Unknown;
}
ColorMatrix mapMatrix(AVColorSpace s) {
    switch (s) {
    case AVCOL_SPC_BT709: return ColorMatrix::BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_FCC: return ColorMatrix::BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return ColorMatrix::BT2020;
    default: return ColorMatrix::Unknown;
    }
}
ColorTransfer mapTransfer(AVColorTransferCharacteristic t) {
    if (t == AVCOL_TRC_SMPTE2084) return ColorTransfer::PQ;
    if (t == AVCOL_TRC_ARIB_STD_B67) return ColorTransfer::HLG;
    return ColorTransfer::SDR;
}
} // namespace

bool isImageExtension(const std::string& path) {
    std::string e = ext(path);
    for (const char* x : kImage)
        if (e == x) return true;
    return false;
}

bool isSupportedExtension(const std::string& path) {
    std::string e = ext(path);
    for (const char* x : kImage)
        if (e == x) return true;
    for (const char* x : kVideo)
        if (e == x) return true;
    for (const char* x : kAudio)
        if (e == x) return true;
    return false;
}

std::string mediaFileFilter() {
    std::string s = "Media files (";
    auto add = [&](const char* const* arr, size_t n) {
        for (size_t i = 0; i < n; ++i) s += std::string("*.") + arr[i] + " ";
    };
    add(kVideo, std::size(kVideo));
    add(kAudio, std::size(kAudio));
    add(kImage, std::size(kImage));
    s.back() = ')';
    s += ";;All files (*.*)";
    return s;
}

MediaInfo probeMedia(const std::string& path) {
    MediaInfo m;
    int64_t size = 0, mtime = 0;
    if (!fileStat(path, size, mtime)) {
        m.error = "File not found";
        return m;
    }
    m.fileSize = size;
    m.mtime = mtime;
    std::string err;
    ff::FormatPtr fmt = ff::openInput(path, err);
    if (!fmt) {
        m.error = err;
        return m;
    }
    m.container = fmt->iformat ? fmt->iformat->name : "";
    // Demuxers that "open" arbitrary text/binary files are not media.
    for (const char* bad : {"tty", "bin", "data", "txt"})
        if (m.container == bad) {
            m.error = "Not a media file";
            return m;
        }
    const bool imageDemuxer = m.container.find("image2") != std::string::npos ||
                              m.container.find("_pipe") != std::string::npos || isImageExtension(path);

    int vIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            if (vIdx < 0 || st->codecpar->width * st->codecpar->height >
                                fmt->streams[vIdx]->codecpar->width * fmt->streams[vIdx]->codecpar->height)
                vIdx = int(i);
        }
    }
    const Tick fmtDur = fmt->duration > 0 ? streamToTicks(fmt->duration, {1, AV_TIME_BASE}) : 0;
    if (vIdx >= 0) {
        AVStream* st = fmt->streams[vIdx];
        AVCodecParameters* cp = st->codecpar;
        auto& v = m.video;
        v.index = vIdx;
        v.width = cp->width;
        v.height = cp->height;
        v.timeBase = ff::rat(st->time_base);
        const AVCodecDescriptor* cd = avcodec_descriptor_get(cp->codec_id);
        v.codec = cd ? cd->name : "unknown";
        const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(AVPixelFormat(cp->format));
        v.pixFmt = pd ? pd->name : "";
        v.bitDepth = pd ? pd->comp[0].depth : 8;
        v.hasAlpha = pd && (pd->flags & AV_PIX_FMT_FLAG_ALPHA);
        v.range = mapRange(cp->color_range);
        if (pd && (pd->flags & AV_PIX_FMT_FLAG_RGB)) v.range = ColorRange::Full;
        if (cp->format == AV_PIX_FMT_YUVJ420P || cp->format == AV_PIX_FMT_YUVJ422P || cp->format == AV_PIX_FMT_YUVJ444P)
            v.range = ColorRange::Full;
        v.matrix = mapMatrix(cp->color_space);
        v.transfer = mapTransfer(cp->color_trc);
        v.startTime = st->start_time != AV_NOPTS_VALUE ? streamToTicks(st->start_time, v.timeBase) : 0;
        v.duration = st->duration > 0 ? streamToTicks(st->duration, v.timeBase) : fmtDur;
        AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        v.fps = fr.num > 0 && fr.den > 0 ? Rational{fr.num, fr.den} : Rational{30, 1};

        m.isImage = imageDemuxer || (st->nb_frames == 1) ||
                    (cp->codec_id == AV_CODEC_ID_PNG || cp->codec_id == AV_CODEC_ID_MJPEG ||
                     cp->codec_id == AV_CODEC_ID_BMP || cp->codec_id == AV_CODEC_ID_WEBP ||
                     cp->codec_id == AV_CODEC_ID_TIFF) && fmtDur <= kTicksPerSecond / 10;
        m.hasVideo = true;

        if (!m.isImage) {
            // Sample packet timestamps to detect VFR and refine the frame rate.
            std::vector<int64_t> pts;
            ff::PacketPtr pkt = ff::makePacket();
            int guard = 0;
            while (pts.size() < 240 && guard++ < 6000 && av_read_frame(fmt.get(), pkt.get()) >= 0) {
                if (pkt->stream_index == vIdx && pkt->pts != AV_NOPTS_VALUE) pts.push_back(pkt->pts);
                av_packet_unref(pkt.get());
            }
            std::sort(pts.begin(), pts.end());
            // With B-frames the newest packets in decode order leave holes at the end of the sorted
            // window (their B-frames are not read yet): ignore the tail.
            if (pts.size() > 40) pts.resize(pts.size() - 16);
            if (pts.size() > 10) {
                std::vector<int64_t> d;
                for (size_t i = 1; i < pts.size(); ++i) d.push_back(pts[i] - pts[i - 1]);
                std::vector<int64_t> s = d;
                std::sort(s.begin(), s.end());
                int64_t med = s[s.size() / 2];
                int64_t mn = s.front(), mx = s.back();
                // Allow +-1 tick of jitter (ms-rounded MKV timestamps alternate 16/17 ms).
                if (med > 0 && (mx > med + std::max<int64_t>(1, med / 4) || mn < med - std::max<int64_t>(1, med / 4)))
                    v.vfr = true;
                if (fr.num <= 0 && med > 0) v.fps = {v.timeBase.den, v.timeBase.num * med};
            }
        } else {
            v.duration = 0;
        }
    }

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (!avcodec_find_decoder(st->codecpar->codec_id)) continue;
        AudioStreamInfo a;
        a.index = int(i);
        a.sampleRate = st->codecpar->sample_rate;
        a.channels = st->codecpar->ch_layout.nb_channels;
        const AVCodecDescriptor* cd = avcodec_descriptor_get(st->codecpar->codec_id);
        a.codec = cd ? cd->name : "unknown";
        if (AVDictionaryEntry* t = av_dict_get(st->metadata, "title", nullptr, 0)) a.title = t->value;
        if (AVDictionaryEntry* l = av_dict_get(st->metadata, "language", nullptr, 0)) a.language = l->value;
        Rational tb = ff::rat(st->time_base);
        a.startTime = st->start_time != AV_NOPTS_VALUE ? streamToTicks(st->start_time, tb) : 0;
        a.duration = st->duration > 0 ? streamToTicks(st->duration, tb) : fmtDur;
        m.audio.push_back(a);
    }

    if (!m.hasVideo && m.audio.empty()) {
        m.error = "No playable video or audio stream";
        return m;
    }
    if (m.hasVideo && !m.isImage && !avcodec_find_decoder(fmt->streams[vIdx]->codecpar->codec_id)) {
        m.error = "Unsupported video codec: " + m.video.codec;
        return m;
    }
    // Normalise: media time 0 = earliest stream start.
    Tick start = INT64_MAX;
    if (m.hasVideo && !m.isImage) start = std::min(start, m.video.startTime);
    for (const auto& a : m.audio) start = std::min(start, a.startTime);
    if (start == INT64_MAX) start = 0;
    Tick dur = 0;
    if (m.hasVideo && !m.isImage) dur = std::max(dur, m.video.startTime - start + m.video.duration);
    for (const auto& a : m.audio) dur = std::max(dur, a.startTime - start + a.duration);
    if (dur <= 0) dur = fmtDur;
    m.duration = m.isImage ? 0 : dur;
    m.origin = start;
    if (!m.isImage && m.duration <= 0) {
        m.error = "Could not determine the media duration";
        return m;
    }
    m.valid = true;
    return m;
}

} // namespace cut
