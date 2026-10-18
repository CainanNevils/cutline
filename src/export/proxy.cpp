#include "export/proxy.h"

#include "export/encoder.h"
#include "media/video_decoder.h"
#include "render/compositor.h"

#include <filesystem>

namespace cut {

int proxyHeight() { return 720; }

bool generateProxy(const Asset& a, const std::string& outPath, JobToken& tok, std::string& error) {
    if (!a.info.hasVideo || a.info.isImage) {
        error = "No video to proxy";
        return false;
    }
    auto gpu = GpuDevice::create(false, error);  // own device: never contends with the preview
    if (!gpu) return false;
    Compositor comp;
    if (!comp.init(gpu, error)) return false;
    VideoDecoder dec;
    if (!dec.open(a.path, gpu, a.info.origin, {}, error)) return false;
    const int H = std::min(proxyHeight(), a.info.video.height);
    const int W = std::max(2, int(std::lround(double(H) * a.info.video.width / a.info.video.height)) & ~1);
    const int Hh = H & ~1;
    VideoEncodeConfig cfg;
    cfg.width = W;
    cfg.height = Hh;
    cfg.fps = a.info.video.fps.valid() ? a.info.video.fps : Rational{30, 1};
    cfg.timeBase = {1, 90000};
    cfg.gop = 1;
    cfg.bframes = 0;
    cfg.lookahead = 0;
    cfg.cq = 24;
    cfg.preset = "p3";
    cfg.tuneHq = false;
    VideoEncoder enc;
    std::string e1;
    cfg.codec = "h264_nvenc";
    if (!enc.open(cfg, gpu, e1)) {
        cfg.codec = "libx264";
        cfg.cq = 23;
        if (!enc.open(cfg, gpu, error)) {
            error = e1 + "; " + error;
            return false;
        }
    }
    std::string tmp = outPath + ".tmp.mp4";
    Muxer mux;
    if (!mux.open(tmp, error)) return false;
    const int vs = mux.addStream(enc.context(), true);
    if (!mux.writeHeader(error)) {
        mux.abort();
        return false;
    }
    enc.setPacketSink([&](AVPacket* p) { return mux.write(p, vs, enc.context()); });
    auto canvas = comp.makeCanvas(W, Hh, false);
    auto nv12 = gpu->acquire(W, Hh, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET);
    std::vector<uint8_t> rb;
    DecodedPicture p;
    int64_t lastPts = -1;
    while (dec.next(INT64_MIN, p)) {
        if (!tok.checkpoint()) {
            mux.abort();
            error = "cancelled";
            return false;
        }
        if (!p.frame) continue;
        int64_t pts = mulDivRound(std::max<Tick>(0, p.pts), 90000, kTicksPerSecond);
        if (pts <= lastPts) continue;  // drop duplicate timestamps
        lastPts = pts;
        comp.clear(*canvas, 0, 0, 0, 1);
        LayerParams lp;
        lp.frame = p.frame.get();
        lp.corners = computeCorners(p.frame->width, p.frame->height, W, Hh, FitMode::Fill, {});
        comp.drawLayer(*canvas, W, Hh, lp);
        comp.toNV12(*canvas, *nv12, ColorMatrix::BT709);
        bool ok;
        if (enc.zeroCopy()) ok = enc.encodeTexture(*nv12, pts);
        else {
            int pitch = 0;
            comp.readback(*nv12, rb, pitch);
            ok = enc.encodeNV12(rb.data(), pitch, rb.data() + size_t(pitch) * Hh, pitch, pts);
        }
        if (!ok) {
            mux.abort();
            error = "encode failed";
            return false;
        }
        if (a.info.duration > 0) tok.progress = double(p.pts) / double(a.info.duration);
    }
    enc.flush();
    if (!mux.finish(error)) {
        mux.abort();
        return false;
    }
    std::error_code ec;
    auto u = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
    std::filesystem::rename(u(tmp), u(outPath), ec);
    if (ec) {
        error = "Could not finalize proxy file";
        return false;
    }
    tok.progress = 1;
    return true;
}

} // namespace cut
