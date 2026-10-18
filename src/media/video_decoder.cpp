#include "media/video_decoder.h"

#include "media/ff.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <thread>

namespace cut {

AVBufferRef* ffHwDeviceFor(GpuDevice& g) {
    std::lock_guard<std::mutex> l(g.ffMutex);
    if (g.ffHwDevice) return static_cast<AVBufferRef*>(g.ffHwDevice);
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return nullptr;
    auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(ref->data)->hwctx);
    hw->device = g.device();
    hw->device->AddRef();
    hw->device_context = g.context();
    hw->device_context->AddRef();
    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        return nullptr;
    }
    g.ffHwDevice = ref;
    g.ffHwDeviceFree = [](void* p) {
        AVBufferRef* r = static_cast<AVBufferRef*>(p);
        av_buffer_unref(&r);
    };
    return ref;
}

namespace {
bool hasD3D11Config(const AVCodec* codec) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* c = avcodec_get_hw_config(codec, i);
        if (!c) return false;
        if (c->device_type == AV_HWDEVICE_TYPE_D3D11VA && (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
    }
}

// Does the GPU decode this codec? (FFmpeg's native AV1 decoder has no software path, so it is only
// picked over libdav1d when the driver offers the AV1 profile.)
bool gpuHasProfile(GpuDevice& g, const GUID& profile) {
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vd;
    if (FAILED(g.device()->QueryInterface(IID_PPV_ARGS(&vd)))) return false;
    const UINT n = vd->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < n; ++i) {
        GUID p;
        if (SUCCEEDED(vd->GetVideoDecoderProfile(i, &p)) && p == profile) return true;
    }
    return false;
}
const GUID kAv1Profile0 = {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};  // AV1 VLD profile 0

AVPixelFormat pickFormat(AVCodecContext* c, const AVPixelFormat* fmts) {
    const bool wantHw = c->opaque != nullptr;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11 && wantHw) return *p;
    // first software format
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

ColorInfo colorOf(const AVFrame* f, int height) {
    ColorInfo c;
    c.range = f->color_range == AVCOL_RANGE_JPEG ? ColorRange::Full : ColorRange::Limited;
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: c.matrix = ColorMatrix::BT709; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_FCC: c.matrix = ColorMatrix::BT601; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: c.matrix = ColorMatrix::BT2020; break;
    default: c.matrix = height >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601; break;  // HD+ default
    }
    if (f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P)
        c.range = ColorRange::Full;
    if (f->color_trc == AVCOL_TRC_SMPTE2084) c.transfer = ColorTransfer::PQ;
    else if (f->color_trc == AVCOL_TRC_ARIB_STD_B67) c.transfer = ColorTransfer::HLG;
    return c;
}

int swsColorspace(ColorMatrix m) {
    switch (m) {
    case ColorMatrix::BT601: return SWS_CS_ITU601;
    case ColorMatrix::BT2020: return SWS_CS_BT2020;
    default: return SWS_CS_ITU709;
    }
}
} // namespace

struct VideoDecoder::Impl {
    std::shared_ptr<GpuDevice> gpu;
    ff::FormatPtr fmt;
    ff::CodecPtr cc;
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr frame = ff::makeFrame();
    int stream = -1;
    Rational tb{1, 1};
    Tick origin = 0;
    bool sentEof = false;
    std::string codecName;
    // software upload
    ff::SwsPtr sws;
    int swsW = 0, swsH = 0, swsSrcFmt = -1, swsDstFmt = -1;
    int swsCs = -1, swsRange = -1;
    std::vector<uint8_t> conv;
    std::vector<ComPtr<ID3D11Texture2D>> staging;
    size_t stagingIdx = 0;
    int stagingW = 0, stagingH = 0;
    DXGI_FORMAT stagingFmt = DXGI_FORMAT_UNKNOWN;
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::isOpen() const { return d_ && d_->cc; }

void VideoDecoder::close() { d_.reset(); }

std::string VideoDecoder::describe() const {
    if (!d_) return "closed";
    return d_->codecName + " " + std::to_string(width_) + "x" + std::to_string(height_) + (hw_ ? " (GPU)" : " (CPU)");
}

bool VideoDecoder::open(const std::string& path, std::shared_ptr<GpuDevice> gpu, Tick origin, const Options& opt,
                        std::string& error) {
    close();
    auto d = std::make_unique<Impl>();
    d->gpu = std::move(gpu);
    d->origin = origin;
    d->fmt = ff::openInput(path, error);
    if (!d->fmt) return false;
    int idx = -1;
    for (unsigned i = 0; i < d->fmt->nb_streams; ++i) {
        AVStream* st = d->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            if (idx < 0 || st->codecpar->width * st->codecpar->height >
                               d->fmt->streams[idx]->codecpar->width * d->fmt->streams[idx]->codecpar->height)
                idx = int(i);
        }
    }
    if (idx < 0) {
        error = "No video stream";
        return false;
    }
    for (unsigned i = 0; i < d->fmt->nb_streams; ++i)
        d->fmt->streams[i]->discard = int(i) == idx ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    AVStream* st = d->fmt->streams[idx];
    d->stream = idx;
    d->tb = ff::rat(st->time_base);
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    // the default decoder may be a software-only library (libdav1d for AV1): prefer a decoder of
    // the same codec that the GPU can run
    if (codec && opt.allowHw && d->gpu && !hasD3D11Config(codec) && ffHwDeviceFor(*d->gpu) &&
        (st->codecpar->codec_id != AV_CODEC_ID_AV1 || gpuHasProfile(*d->gpu, kAv1Profile0))) {
        void* it = nullptr;
        while (const AVCodec* c = av_codec_iterate(&it))
            if (av_codec_is_decoder(c) && c->id == st->codecpar->codec_id && hasD3D11Config(c)) {
                codec = c;
                break;
            }
    }
    if (!codec) {
        error = std::string("Unsupported video codec ") + avcodec_get_name(st->codecpar->codec_id);
        return false;
    }
    d->codecName = codec->name;
    d->cc.reset(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(d->cc.get(), st->codecpar);
    d->cc->pkt_timebase = st->time_base;

    bool tryHw = opt.allowHw && d->gpu;
    if (tryHw) {
        // Only codecs with a D3D11VA hwaccel.
        const bool hasHw = hasD3D11Config(codec);
        AVBufferRef* dev = hasHw ? ffHwDeviceFor(*d->gpu) : nullptr;
        if (dev) {
            d->cc->hw_device_ctx = av_buffer_ref(dev);
            d->cc->opaque = this;  // marks "hw wanted" for pickFormat
            d->cc->get_format = pickFormat;
            d->cc->thread_count = 1;
            d->cc->extra_hw_frames = 4;
        } else tryHw = false;
    }
    if (!tryHw) {
        d->cc->thread_count = opt.swThreads > 0 ? opt.swThreads : std::min(16, int(std::thread::hardware_concurrency()));
        d->cc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    int r = avcodec_open2(d->cc.get(), codec, nullptr);
    if (r < 0) {
        error = "Cannot open decoder: " + ff::err(r);
        return false;
    }
    width_ = st->codecpar->width;
    height_ = st->codecpar->height;
    AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    frameDur_ = fr.num > 0 ? mulDivRound(kTicksPerSecond, fr.den, fr.num) : kTicksPerSecond / 30;
    hw_ = tryHw;  // refined after the first frame (driver may still refuse the profile)
    eof_ = false;
    decoded_ = 0;
    d_ = std::move(d);
    return true;
}

bool VideoDecoder::seek(Tick t) {
    if (!d_) return false;
    int64_t ts = ticksToStream(std::max<Tick>(0, t) + d_->origin, d_->tb);
    int r = av_seek_frame(d_->fmt.get(), d_->stream, ts, AVSEEK_FLAG_BACKWARD);
    if (r < 0) r = avformat_seek_file(d_->fmt.get(), d_->stream, INT64_MIN, ts, ts, 0);
    if (r < 0) r = av_seek_frame(d_->fmt.get(), d_->stream, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(d_->cc.get());
    d_->sentEof = false;
    eof_ = false;
    return r >= 0;
}

bool VideoDecoder::next(Tick keepFrom, DecodedPicture& out) {
    if (!d_ || eof_) return false;
    Impl& d = *d_;
    AVFrame* f = d.frame.get();
    for (;;) {
        int r = avcodec_receive_frame(d.cc.get(), f);
        if (r == 0) break;
        if (r == AVERROR_EOF) {
            eof_ = true;
            return false;
        }
        if (r != AVERROR(EAGAIN)) {
            // decoder error: try to continue with the next packet
        }
        if (d.sentEof) {
            eof_ = true;
            return false;
        }
        int rr = av_read_frame(d.fmt.get(), d.pkt.get());
        if (rr < 0) {
            avcodec_send_packet(d.cc.get(), nullptr);
            d.sentEof = true;
            continue;
        }
        if (d.pkt->stream_index == d.stream) avcodec_send_packet(d.cc.get(), d.pkt.get());
        av_packet_unref(d.pkt.get());
    }
    ++decoded_;
    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    out.pts = pts == AV_NOPTS_VALUE ? 0 : streamToTicks(pts, d.tb) - d.origin;
    out.duration = f->duration > 0 ? streamToTicks(f->duration, d.tb) : frameDur_;
    out.frame.reset();
    if (out.pts < keepFrom) {
        av_frame_unref(f);
        return true;
    }
    auto gf = std::make_shared<GpuFrame>();
    gf->pts = out.pts;
    gf->end = out.pts + out.duration;
    gf->width = f->width;
    gf->height = f->height;
    gf->color = colorOf(f, f->height);
    ID3D11DeviceContext* ctx = d.gpu->context();
    const int w2 = (f->width + 1) & ~1, h2 = (f->height + 1) & ~1;

    if (f->format == AV_PIX_FMT_D3D11) {
        hw_ = true;
        auto* src = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
        UINT slice = UINT(reinterpret_cast<intptr_t>(f->data[1]));
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        const bool p010 = sd.Format == DXGI_FORMAT_P010;
        gf->layout = p010 ? PixelLayout::P010 : PixelLayout::NV12;
        gf->tex = d.gpu->acquire(w2, h2, sd.Format, D3D11_BIND_SHADER_RESOURCE);
        if (!gf->tex) {
            av_frame_unref(f);
            return true;
        }
        D3D11_BOX box{0, 0, 0, UINT(std::min<int>(w2, int(sd.Width))), UINT(std::min<int>(h2, int(sd.Height))), 1};
        ctx->CopySubresourceRegion(gf->tex->tex.Get(), 0, 0, 0, 0, src, D3D11CalcSubresource(0, slice, 1), &box);
        gf->hw = true;
        av_frame_unref(f);
        out.frame = std::move(gf);
        return true;
    }

    hw_ = false;
    // Software frame -> NV12 / P010 (shader converts) or BGRA (alpha / RGB / exotic formats).
    const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    const bool hasAlpha = pd && (pd->flags & AV_PIX_FMT_FLAG_ALPHA);
    const bool isRgb = pd && (pd->flags & AV_PIX_FMT_FLAG_RGB);
    const bool isPal = pd && (pd->flags & AV_PIX_FMT_FLAG_PAL);
    const int depth = pd ? pd->comp[0].depth : 8;
    AVPixelFormat dst;
    DXGI_FORMAT dxf;
    if (hasAlpha || isRgb || isPal || f->format == AV_PIX_FMT_GRAY8) {
        dst = AV_PIX_FMT_BGRA;
        dxf = DXGI_FORMAT_B8G8R8A8_UNORM;
        gf->layout = PixelLayout::BGRA;
    } else if (depth > 8) {
        dst = AV_PIX_FMT_P010LE;
        dxf = DXGI_FORMAT_P010;
        gf->layout = PixelLayout::P010;
    } else {
        dst = AV_PIX_FMT_NV12;
        dxf = DXGI_FORMAT_NV12;
        gf->layout = PixelLayout::NV12;
    }
    const int tw = dxf == DXGI_FORMAT_B8G8R8A8_UNORM ? f->width : w2;
    const int th = dxf == DXGI_FORMAT_B8G8R8A8_UNORM ? f->height : h2;

    // staging ring (3 deep so we never Map a texture the GPU is still copying from)
    if (d.stagingW != tw || d.stagingH != th || d.stagingFmt != dxf) {
        d.staging.clear();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(tw);
        td.Height = UINT(th);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxf;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (int i = 0; i < 3; ++i) {
            ComPtr<ID3D11Texture2D> t;
            if (FAILED(d.gpu->device()->CreateTexture2D(&td, nullptr, &t))) return false;
            d.staging.push_back(t);
        }
        d.stagingW = tw;
        d.stagingH = th;
        d.stagingFmt = dxf;
    }
    ID3D11Texture2D* stg = d.staging[d.stagingIdx++ % d.staging.size()].Get();
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_WRITE, 0, &map))) {
        av_frame_unref(f);
        return true;
    }
    uint8_t* dstData[4] = {static_cast<uint8_t*>(map.pData), nullptr, nullptr, nullptr};
    int dstLines[4] = {int(map.RowPitch), 0, 0, 0};
    if (dxf != DXGI_FORMAT_B8G8R8A8_UNORM) {
        dstData[1] = dstData[0] + size_t(map.RowPitch) * th;  // UV plane follows Y with the same pitch
        dstLines[1] = int(map.RowPitch);
    }
    ColorInfo ci = gf->color;
    int cs = swsColorspace(ci.matrix);
    int srcRange = ci.range == ColorRange::Full ? 1 : 0;
    if (!d.sws || d.swsW != f->width || d.swsH != f->height || d.swsSrcFmt != f->format || d.swsDstFmt != dst ||
        d.swsCs != cs || d.swsRange != srcRange) {
        SwsContext* s = sws_alloc_context();
        av_opt_set_int(s, "srcw", f->width, 0);
        av_opt_set_int(s, "srch", f->height, 0);
        av_opt_set_int(s, "src_format", f->format, 0);
        av_opt_set_int(s, "dstw", f->width, 0);
        av_opt_set_int(s, "dsth", f->height, 0);
        av_opt_set_int(s, "dst_format", dst, 0);
        av_opt_set_int(s, "sws_flags", SWS_POINT, 0);
        av_opt_set_int(s, "threads", 0, 0);
        if (sws_init_context(s, nullptr, nullptr) < 0) {
            sws_freeContext(s);
            ctx->Unmap(stg, 0);
            av_frame_unref(f);
            return true;
        }
        // YUV->RGB conversions need the right matrix/range; YUV->YUV keeps range (shader expands).
        if (dst == AV_PIX_FMT_BGRA && !isRgb) {
            const int* coef = sws_getCoefficients(cs);
            sws_setColorspaceDetails(s, coef, srcRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        }
        d.sws.reset(s);
        d.swsW = f->width;
        d.swsH = f->height;
        d.swsSrcFmt = f->format;
        d.swsDstFmt = dst;
        d.swsCs = cs;
        d.swsRange = srcRange;
    }
    sws_scale(d.sws.get(), f->data, f->linesize, 0, f->height, dstData, dstLines);
    ctx->Unmap(stg, 0);
    if (dst == AV_PIX_FMT_BGRA) {
        gf->color.range = ColorRange::Full;  // already RGB
    }
    gf->tex = d.gpu->acquire(tw, th, dxf, D3D11_BIND_SHADER_RESOURCE);
    if (gf->tex) ctx->CopyResource(gf->tex->tex.Get(), stg);
    av_frame_unref(f);
    if (gf->tex) out.frame = std::move(gf);
    return true;
}

} // namespace cut
