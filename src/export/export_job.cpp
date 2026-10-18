#include "export/export_job.h"

#include "audio/loudness.h"
#include "audio/mixer.h"
#include "core/project_io.h"
#include "core/srt.h"
#include "engine/frame_renderer.h"
#include "engine/sync_provider.h"
#include "media/ff.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>

namespace cut {

ExportJob::ExportJob(SnapshotPtr snap, ExportSettings s) : snap_(std::move(snap)), s_(std::move(s)) {}

ExportJob::~ExportJob() {
    cancel();
    wait();
}

void ExportJob::start() {
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void ExportJob::wait() {
    if (thread_.joinable()) thread_.join();
}

bool ExportJob::enoughDiskSpace(const ExportSettings& s, uint64_t& need, uint64_t& freeBytes) {
    double secs = ticksToSeconds(s.rangeEnd - s.rangeStart);
    double bps = double(s.video.cq > 0 ? s.video.maxBitrate : s.video.maxBitrate) + (s.audio ? s.audioBitrate : 0);
    need = uint64_t(secs * bps / 8.0 * 1.15) + (16u << 20);
    std::filesystem::path p(std::u8string(s.path.begin(), s.path.end()));
    std::error_code ec;
    auto info = std::filesystem::space(p.parent_path().empty() ? std::filesystem::current_path() : p.parent_path(), ec);
    if (ec) {
        freeBytes = UINT64_MAX;
        return true;
    }
    freeBytes = info.available;
    return freeBytes > need;
}

bool ExportJob::analyzeLoudness() {
    Mixer mixer;
    LoudnessMeter meter;
    const int64_t s0 = ticksToSamples(s_.rangeStart, 48000);
    const int64_t s1 = ticksToSamples(s_.rangeEnd, 48000);
    const int block = 4800;
    std::vector<float> buf(size_t(block) * 2);
    st_.analyzeProgress = 0;
    for (int64_t s = s0; s < s1; s += block) {
        if (cancel_) return false;
        const int n = int(std::min<int64_t>(block, s1 - s));
        mixer.render(*snap_, s, buf.data(), n);
        meter.push(buf.data(), n);
        st_.analyzeProgress = double(s - s0) / double(std::max<int64_t>(1, s1 - s0));
    }
    st_.analyzeProgress = 1;
    const double lufs = meter.integrated();
    double gainDb = 0;
    if (std::isfinite(lufs)) {
        gainDb = s_.normalizeLufs - lufs;
        const double peak = meter.samplePeak();
        if (peak > 0) gainDb = std::min(gainDb, -1.0 - 20 * std::log10(peak));  // keep peaks under -1 dBFS
        gainDb = std::clamp(gainDb, -40.0, 40.0);
    }
    gain_ = float(std::pow(10.0, gainDb / 20));
    std::lock_guard<std::mutex> l(st_.m);
    st_.measuredLufs = std::isfinite(lufs) ? lufs : -70;
    st_.appliedGainDb = gainDb;
    return true;
}

bool ExportJob::runAudio(Muxer& mux, int stream, AudioEncoder& enc) {
    Mixer mixer;
    const int64_t s0 = ticksToSamples(s_.rangeStart, 48000);
    const int64_t s1 = ticksToSamples(s_.rangeEnd, 48000);
    const int block = 1024;
    std::vector<float> buf(size_t(block) * 2);
    enc.setPacketSink([&](AVPacket* p) { return mux.write(p, stream, enc.context()); });
    const Tick fd = frameDuration(s_.fps.valid() ? s_.fps : snap_->seq.settings.fps);
    for (int64_t s = s0; s < s1; s += block) {
        if (cancel_) return false;
        // keep audio within ~2 s of the video so the muxer interleaves without big buffers
        if (!s_.withVideo) st_.framesDone = int64_t(samplesToTicks(s - s0, 48000) / fd);  // audio only: progress
        while (s_.withVideo && !cancel_ && !st_.finished &&
               samplesToTicks(s - s0, 48000) > st_.framesDone.load() * fd + 2 * kTicksPerSecond &&
               st_.framesDone.load() < st_.framesTotal)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        int n = int(std::min<int64_t>(block, s1 - s));
        mixer.render(*snap_, s, buf.data(), n);
        if (gain_ != 1.0f)
            for (int i = 0; i < n * 2; ++i) buf[size_t(i)] *= gain_;
        if (!enc.encode(buf.data(), n)) return false;
    }
    return enc.flush();
}

void ExportJob::runAudioOnly() {
    auto t0 = std::chrono::steady_clock::now();
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> l(st_.m);
        st_.error = e;
        st_.failed = true;
    };
    const Rational fps = s_.fps.valid() ? s_.fps : snap_->seq.settings.fps;
    st_.framesTotal = std::max<int64_t>(1, ticksToFrame(s_.rangeEnd - s_.rangeStart + frameDuration(fps) - 1, fps));
    {
        std::lock_guard<std::mutex> l(st_.m);
        st_.videoPath = s_.path;
        st_.encoder = s_.audioCodec + " (audio only)";
    }
    std::string err;
    bool ok = true;
    if (s_.normalizeLufs != 0 && !analyzeLoudness()) {
        st_.cancelled = true;
        ok = false;
    }
    AudioEncoder aenc;
    Muxer mux;
    if (ok && !aenc.open(s_.audioBitrate, err, s_.audioCodec)) fail(err), ok = false;
    if (ok && !mux.open(s_.path, err)) fail(err), ok = false;
    const bool m4a = s_.path.size() > 4 && _stricmp(s_.path.c_str() + s_.path.size() - 4, ".m4a") == 0;
    const int as = ok ? mux.addStream(aenc.context(), m4a) : -1;
    if (ok && !mux.writeHeader(err)) fail(err), ok = false;
    if (ok && !runAudio(mux, as, aenc)) {
        ok = false;
        if (!cancel_) fail("Audio encoding failed");
    }
    if (ok && !mux.finish(err)) fail(err + " (disk full?)"), ok = false;
    if (!ok) {
        mux.abort();
        if (cancel_) st_.cancelled = true;
    } else {
        st_.framesDone = st_.framesTotal;
    }
    st_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (st_.seconds > 0) st_.fps = double(st_.framesDone.load()) / st_.seconds;
    running_ = false;
    st_.finished = true;
}

void ExportJob::run() {
    if (!s_.withVideo) {
        runAudioOnly();
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> l(st_.m);
        st_.error = e;
        st_.failed = true;
    };
    const Sequence& seq = snap_->seq;
    const Rational fps = s_.fps.valid() ? s_.fps : seq.settings.fps;
    const int64_t f0 = ticksToFrame(s_.rangeStart, fps);
    const int64_t f1 = ticksToFrame(s_.rangeEnd + frameDuration(fps) - 1, fps);  // ceil
    st_.framesTotal = std::max<int64_t>(0, f1 - f0);
    {
        std::lock_guard<std::mutex> l(st_.m);
        st_.videoPath = s_.path;
    }
    std::string err;
    auto gpu = GpuDevice::create(false, err);
    if (!gpu) {
        fail(err);
        running_ = false;
        st_.finished = true;
        return;
    }
    Compositor comp;
    if (!comp.init(gpu, err)) {
        fail(err);
        running_ = false;
        st_.finished = true;
        return;
    }
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, s_.allowHwDecode);

    VideoEncodeConfig vc = s_.video;
    vc.width = s_.width & ~1;
    vc.height = s_.height & ~1;
    vc.fps = fps;
    VideoEncoder venc;
    if (!venc.open(vc, gpu, err)) {
        if (!s_.allowFallback) {
            fail(err);
            running_ = false;
            st_.finished = true;
            return;
        }
        std::string e1 = err;
        vc.codec = "libx264";
        if (!venc.open(vc, gpu, err)) {
            fail(e1 + "; " + err);
            running_ = false;
            st_.finished = true;
            return;
        }
    }
    {
        std::lock_guard<std::mutex> l(st_.m);
        st_.encoder = venc.describe();
    }
    if (s_.audio && s_.normalizeLufs != 0 && !analyzeLoudness()) {
        st_.cancelled = true;
        running_ = false;
        st_.finished = true;
        return;
    }
    AudioEncoder aenc;
    if (s_.audio && !aenc.open(s_.audioBitrate, err)) {
        fail(err);
        running_ = false;
        st_.finished = true;
        return;
    }
    Muxer mux;
    if (!mux.open(s_.path, err)) {
        fail(err);
        running_ = false;
        st_.finished = true;
        return;
    }
    const bool mp4 = s_.path.size() > 4 &&
                     (_stricmp(s_.path.c_str() + s_.path.size() - 4, ".mp4") == 0 ||
                      _stricmp(s_.path.c_str() + s_.path.size() - 4, ".mov") == 0);
    const int vs = mux.addStream(venc.context(), mp4);
    const int as = s_.audio ? mux.addStream(aenc.context(), mp4) : -1;
    if (!mux.writeHeader(err)) {
        fail(err);
        mux.abort();
        running_ = false;
        st_.finished = true;
        return;
    }
    venc.setPacketSink([&](AVPacket* p) { return mux.write(p, vs, venc.context()); });

    // audio runs concurrently (CPU), video on this thread (GPU)
    std::atomic<bool> audioOk{true};
    std::thread audioThread;
    if (s_.audio) audioThread = std::thread([&] { audioOk = runAudio(mux, as, aenc); });

    RenderSnapshot snap = *snap_;
    snap.useProxies = false;
    snap.showCaptions = s_.burnCaptions;
    auto canvas = comp.makeCanvas(vc.width, vc.height, s_.highQuality);
    auto nv12 = gpu->acquire(vc.width, vc.height, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET);
    std::vector<uint8_t> rb;
    bool ok = canvas && nv12;
    auto lastFpsT = std::chrono::steady_clock::now();
    int64_t lastFpsF = 0;
    for (int64_t f = f0; ok && f < f1; ++f) {
        if (cancel_) {
            ok = false;
            break;
        }
        const Tick t = frameToTicks(f, fps);
        renderer.render(snap, t, *canvas, provider, s_.burnCaptions);
        comp.toNV12(*canvas, *nv12, vc.matrix);
        const int64_t idx = f - f0;
        if (venc.zeroCopy()) ok = venc.encodeTexture(*nv12, idx);
        else {
            int pitch = 0;
            ok = comp.readback(*nv12, rb, pitch) &&
                 venc.encodeNV12(rb.data(), pitch, rb.data() + size_t(pitch) * vc.height, pitch, idx);
        }
        if (!ok) fail("Video encoding failed at frame " + std::to_string(idx));
        st_.framesDone = idx + 1;
        if ((idx & 31) == 0) {
            // close decoders for clips that are behind us
            std::set<Id> keep;
            for (const Clip* c : clipsInRange(seq, t, t + 2 * kTicksPerSecond, TrackKind::Video)) keep.insert(c->id);
            provider.retain(keep);
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - lastFpsT).count();
            if (dt > 0.5) {
                st_.fps = double(idx - lastFpsF) / dt;
                lastFpsT = now;
                lastFpsF = idx;
            }
        }
    }
    if (ok) ok = venc.flush();
    if (audioThread.joinable()) audioThread.join();
    if (ok && !audioOk) {
        ok = false;
        if (!cancel_) fail("Audio encoding failed");
    }
    {
        std::lock_guard<std::mutex> l(st_.m);
        st_.decodeInfo = provider.anySoftware() ? "some clips decoded on the CPU" : "GPU decode";
    }
    if (ok) {
        if (!mux.finish(err)) {
            fail(err + " (disk full?)");
            ok = false;
        }
    }
    if (!ok) {
        mux.abort();
        if (cancel_) st_.cancelled = true;
    } else if (s_.sidecarSrt) {
        std::vector<SrtEntry> entries;
        for (const auto& c : seq.captions) {
            if (c.end <= s_.rangeStart || c.start >= s_.rangeEnd) continue;
            entries.push_back({std::max(c.start, s_.rangeStart) - s_.rangeStart, std::min(c.end, s_.rangeEnd) - s_.rangeStart,
                               c.text});
        }
        std::string srt = s_.path;
        size_t dot = srt.rfind('.');
        srt = (dot == std::string::npos ? srt : srt.substr(0, dot)) + ".srt";
        writeFileAtomic(srt, writeSrt(entries));
    }
    st_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (st_.seconds > 0) st_.fps = double(st_.framesDone.load()) / st_.seconds;
    running_ = false;
    st_.finished = true;
}

} // namespace cut
