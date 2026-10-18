#pragma once
// FFmpeg RAII helpers. Only media/ and export/ include FFmpeg headers.
#include <d3d11.h>  // must precede hwcontext_d3d11va.h, which includes it inside extern "C"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

#include "core/time.h"

namespace cut::ff {

struct FormatDel {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};
struct CodecDel {
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
struct FrameDel {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct PacketDel {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwrDel {
    void operator()(SwrContext* p) const { swr_free(&p); }
};
struct SwsDel {
    void operator()(SwsContext* p) const { sws_freeContext(p); }
};
struct GraphDel {
    void operator()(AVFilterGraph* p) const { avfilter_graph_free(&p); }
};
struct BufDel {
    void operator()(AVBufferRef* p) const { av_buffer_unref(&p); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDel>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDel>;
using FramePtr = std::unique_ptr<AVFrame, FrameDel>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDel>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDel>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDel>;
using GraphPtr = std::unique_ptr<AVFilterGraph, GraphDel>;
using BufPtr = std::unique_ptr<AVBufferRef, BufDel>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

inline std::string err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

inline Rational rat(AVRational r) { return {r.num, r.den}; }
inline AVRational avrat(Rational r) { return {int(r.num), int(r.den)}; }

// Opens a file for demuxing (UTF-8 path). Returns null + error text on failure.
FormatPtr openInput(const std::string& path, std::string& error);

// Quiet FFmpeg's stderr logging unless CUTLINE_FFLOG is set.
void initLogging();

} // namespace cut::ff
