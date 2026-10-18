// mediabench: headless decode / render / encode / audio / seek benchmarks (M1 risk spikes and
// the numbers behind docs/PROGRESS.md). Prints one JSON object per run on stdout.
//
//   mediabench decode   <file> [--sw] [--seconds N]
//   mediabench present  <file> [--sw] [--seconds N] [--vsync]
//   mediabench composite <file> [--layers N] [--frames N] [--hq]
//   mediabench encode   [--codec h264_nvenc] [--w 2560] [--h 1440] [--seconds 20] [--nozerocopy] [--out f.mp4]
//   mediabench audio    <file> [--stream N] [--seconds N]
//   mediabench seek     <file> [--count N] [--sw]
#include "audio/audio_output.h"
#include "core/project_io.h"
#include "engine/media_cache.h"
#include "engine/playback.h"
#include "export/export_job.h"
#include "export/proxy.h"
#include <psapi.h>
#include "audio/ring.h"
#include "export/encoder.h"
#include "media/ff.h"
#include "media/frame_reader.h"
#include "media/probe.h"
#include "media/scene_detect.h"
#include "render/compositor.h"
#include "render/gpu_timer.h"
#include "render/swapchain.h"

#include <QGuiApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

int makeBenchProjects(const std::string& media, const std::string& outDir);  // projects.cpp

using namespace cut;
using Clock = std::chrono::steady_clock;

namespace {

std::string js(const std::string& s);  // JSON string escaping (defined below)

std::map<std::string, std::string> g_opts;
std::vector<std::string> g_pos;

bool flag(const char* k) { return g_opts.count(k) > 0; }
std::string opt(const char* k, const std::string& def) {
    auto it = g_opts.find(k);
    return it == g_opts.end() ? def : it->second;
}
double optd(const char* k, double def) { return flag(k) ? std::stod(g_opts[k]) : def; }
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::shared_ptr<GpuDevice> makeGpu() {
    std::string err;
    auto g = GpuDevice::create(false, err);
    if (!g) {
        std::fprintf(stderr, "GPU: %s\n", err.c_str());
        std::exit(2);
    }
    return g;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

HWND makeWindow(int w, int h) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mediabench";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // Stay out of the way: prefer a secondary monitor (someone may be gaming on the primary one),
    // never take focus, no taskbar button.
    POINT at{50, 50};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(m, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) {
                auto* pt = reinterpret_cast<POINT*>(p);
                pt->x = mi.rcWork.left + 40;
                pt->y = mi.rcWork.top + 40;
                return FALSE;  // first secondary monitor
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&at));
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"mediabench", L"mediabench", WS_OVERLAPPEDWINDOW,
                                at.x, at.y, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

void pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

LayerParams fullFrameLayer(const GpuFrame& f, int W, int H) {
    LayerParams lp;
    lp.frame = &f;
    lp.corners = computeCorners(f.width, f.height, W, H, FitMode::Fit, {});
    return lp;
}

// ---------------------------------------------------------------------------------------------
int cmdDecode(const std::string& path) {
    auto gpu = makeGpu();
    MediaInfo mi = probeMedia(path);
    if (!mi.valid) {
        std::fprintf(stderr, "probe: %s\n", mi.error.c_str());
        return 1;
    }
    VideoDecoder dec;
    VideoDecoder::Options o;
    o.allowHw = !flag("--sw");
    std::string err;
    if (!dec.open(path, gpu, mi.origin, o, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    double limit = optd("--seconds", 1e9);
    size_t vram0 = gpu->vramUsage();
    auto t0 = Clock::now();
    int frames = 0;
    std::vector<GpuFramePtr> keep;  // hold a few like a cache would
    DecodedPicture p;
    while (dec.next(INT64_MIN, p)) {
        ++frames;
        keep.push_back(p.frame);
        if (keep.size() > 16) keep.erase(keep.begin());
        if (ticksToSeconds(p.pts) >= limit) break;
    }
    gpu->context()->Flush();
    double ms = msSince(t0);
    double fps = frames * 1000.0 / ms;
    double srcFps = mi.video.fps.toDouble();
    std::printf("{\"test\":\"decode\",\"file\":\"%s\",\"decoder\":\"%s\",\"frames\":%d,\"ms\":%.1f,\"fps\":%.1f,"
                "\"x_realtime\":%.2f,\"vram_mb\":%.0f}\n",
                mi.video.codec.c_str(), dec.describe().c_str(), frames, ms, fps, fps / srcFps,
                double(gpu->vramUsage() - vram0) / 1048576.0);
    return 0;
}

int cmdPresent(const std::string& path) {
    auto gpu = makeGpu();
    MediaInfo mi = probeMedia(path);
    if (!mi.valid) return 1;
    VideoDecoder dec;
    VideoDecoder::Options o;
    o.allowHw = !flag("--sw");
    std::string err;
    if (!dec.open(path, gpu, mi.origin, o, err)) return 1;
    Compositor comp;
    if (!comp.init(gpu, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const int W = mi.video.width, H = mi.video.height;
    const int ww = 1280, wh = int(1280.0 * H / W);
    HWND hwnd = makeWindow(ww, wh);
    SwapChain sc;
    if (!sc.create(*gpu, hwnd, ww, wh, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    GpuTimer timer;
    timer.init(gpu->device());
    auto canvas = comp.makeCanvas(W, H, false);
    const bool vsync = flag("--vsync");
    double limit = optd("--seconds", 20);
    std::vector<double> cpuMs, gpuMs, decMs;
    auto t0 = Clock::now();
    int frames = 0;
    DecodedPicture p;
    size_t vram0 = gpu->vramUsage();
    while (msSince(t0) < limit * 1000) {
        pump();
        auto f0 = Clock::now();
        if (!dec.next(INT64_MIN, p)) break;
        decMs.push_back(msSince(f0));
        timer.begin(gpu->context());
        comp.clear(*canvas);
        comp.drawLayer(*canvas, W, H, fullFrameLayer(*p.frame, W, H));
        const float dst[4] = {0, 0, float(ww), float(wh)};
        const float bg[4] = {0, 0, 0, 1};
        comp.blit(sc.rtv(), ww, wh, *canvas, dst, bg, false, bg);
        timer.end(gpu->context());
        sc.present(vsync);
        cpuMs.push_back(msSince(f0));
        double g = timer.poll(gpu->context());
        if (g >= 0) gpuMs.push_back(g);
        ++frames;
    }
    double ms = msSince(t0);
    std::printf("{\"test\":\"present\",\"decoder\":\"%s\",\"frames\":%d,\"fps\":%.1f,\"decode_ms_avg\":%.2f,"
                "\"frame_ms_avg\":%.2f,\"frame_ms_p99\":%.2f,\"gpu_ms_avg\":%.3f,\"vram_mb\":%.0f}\n",
                dec.describe().c_str(), frames, frames * 1000.0 / ms,
                decMs.empty() ? 0 : std::accumulate(decMs.begin(), decMs.end(), 0.0) / decMs.size(),
                cpuMs.empty() ? 0 : std::accumulate(cpuMs.begin(), cpuMs.end(), 0.0) / cpuMs.size(), pct(cpuMs, 0.99),
                gpuMs.empty() ? 0 : std::accumulate(gpuMs.begin(), gpuMs.end(), 0.0) / gpuMs.size(),
                double(gpu->vramUsage() - vram0) / 1048576.0);
    DestroyWindow(hwnd);
    return 0;
}

int cmdComposite(const std::string& path) {
    auto gpu = makeGpu();
    MediaInfo mi = probeMedia(path);
    if (!mi.valid) return 1;
    FrameReader rd;
    std::string err;
    if (!rd.open(path, gpu, mi, {}, err)) return 1;
    Compositor comp;
    comp.init(gpu, err);
    const int W = 2560, H = 1440;
    const int layers = int(optd("--layers", 3));
    const int n = int(optd("--frames", 300));
    auto canvas = comp.makeCanvas(W, H, flag("--hq"));
    GpuTimer timer;
    timer.init(gpu->device());
    std::vector<double> g;
    // decode a handful of frames so every layer has its own texture
    std::vector<GpuFramePtr> src;
    for (int i = 0; i < layers; ++i) src.push_back(rd.get(frameDuration(mi.video.fps) * (i * 7)));
    for (int i = 0; i < n; ++i) {
        timer.begin(gpu->context());
        comp.clear(*canvas);
        for (int l = 0; l < layers; ++l) {
            LayerParams lp;
            lp.frame = src[size_t(l)].get();
            MotionValues mv;
            if (l > 0) {
                mv.scale = 35;
                mv.posX = (l == 1 ? -1 : 1) * 700.0;
                mv.posY = -350;
                mv.rotation = l * 3.0;
            }
            lp.corners = computeCorners(lp.frame->width, lp.frame->height, W, H, FitMode::Fill, mv);
            lp.cc.enabled = true;
            lp.cc.exposure = 0.2f;
            lp.cc.contrast = 0.1f;
            lp.cc.saturation = 1.1f;
            lp.opacity = l == 2 ? 0.8f : 1.f;
            if (l == 0) lp.blurPx = float(optd("--blur", 0));  // full-frame gaussian blur cost
            if (l == 0) lp.sharpen = float(optd("--sharpen", 0));
            comp.drawLayer(*canvas, W, H, lp);
        }
        timer.end(gpu->context());
        gpu->context()->Flush();
        uint64_t want = timer.completed() + 1;
        auto w0 = Clock::now();
        while (timer.completed() < want && msSince(w0) < 100) timer.poll(gpu->context());
        if (i > 10) g.push_back(timer.last());
    }
    // drain
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        timer.poll(gpu->context());
    }
    std::printf("{\"test\":\"composite\",\"layers\":%d,\"size\":\"%dx%d\",\"hq\":%s,\"blur_px\":%.0f,\"gpu_ms_avg\":%.3f,"
                "\"gpu_ms_p99\":%.3f}\n",
                layers, W, H, flag("--hq") ? "true" : "false", optd("--blur", 0),
                g.empty() ? -1.0 : std::accumulate(g.begin(), g.end(), 0.0) / g.size(), pct(g, 0.99));
    return 0;
}

int cmdEncode() {
    auto gpu = makeGpu();
    VideoEncodeConfig cfg;
    cfg.codec = opt("--codec", "h264_nvenc");
    cfg.width = int(optd("--w", 2560));
    cfg.height = int(optd("--h", 1440));
    cfg.fps = {60, 1};
    cfg.bitrate = 24'000'000;
    cfg.maxBitrate = 36'000'000;
    cfg.zeroCopy = !flag("--nozerocopy");
    const double seconds = optd("--seconds", 20);
    std::string out = opt("--out", "mediabench_encode.mp4");
    std::string err;
    VideoEncoder enc;
    if (!enc.open(cfg, gpu, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Muxer mux;
    if (!mux.open(out, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    int vs = mux.addStream(enc.context(), true);
    if (!mux.writeHeader(err)) return 1;
    enc.setPacketSink([&](AVPacket* p) { return mux.write(p, vs, enc.context()); });
    Compositor comp;
    comp.init(gpu, err);
    auto canvas = comp.makeCanvas(cfg.width, cfg.height, false);
    auto nv12 = gpu->acquire(cfg.width, cfg.height, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET);
    // test pattern source: gradient image
    std::vector<uint8_t> img(size_t(512) * 512 * 4);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x) {
            uint8_t* px = &img[(size_t(y) * 512 + x) * 4];
            px[0] = uint8_t(x / 2);
            px[1] = uint8_t(y / 2);
            px[2] = uint8_t(255 - x / 2);
            px[3] = 255;
        }
    auto pattern = comp.uploadBGRA(512, 512, img.data(), 512 * 4);
    std::vector<uint8_t> rb;
    const int total = int(seconds * 60);
    auto t0 = Clock::now();
    for (int i = 0; i < total; ++i) {
        comp.clear(*canvas, 0.1f, 0.1f, 0.12f, 1);
        LayerParams lp;
        lp.rgba = pattern.get();
        MotionValues mv;
        mv.posX = std::sin(i * 0.05) * cfg.width * 0.35;
        mv.posY = std::cos(i * 0.03) * cfg.height * 0.3;
        mv.rotation = i * 0.5;
        lp.corners = computeCorners(512, 512, cfg.width, cfg.height, FitMode::None, mv);
        comp.drawLayer(*canvas, cfg.width, cfg.height, lp);
        comp.toNV12(*canvas, *nv12, ColorMatrix::BT709);
        bool ok;
        if (enc.zeroCopy()) ok = enc.encodeTexture(*nv12, i);
        else {
            int pitch = 0;
            comp.readback(*nv12, rb, pitch);
            ok = enc.encodeNV12(rb.data(), pitch, rb.data() + size_t(pitch) * cfg.height, pitch, i);
        }
        if (!ok) {
            std::fprintf(stderr, "encode failed at frame %d\n", i);
            return 1;
        }
    }
    enc.flush();
    mux.finish(err);
    double ms = msSince(t0);
    double fps = total * 1000.0 / ms;
    std::printf("{\"test\":\"encode\",\"encoder\":\"%s\",\"size\":\"%dx%d\",\"frames\":%d,\"fps\":%.1f,"
                "\"x_realtime\":%.2f,\"out\":\"%s\"}\n",
                enc.describe().c_str(), cfg.width, cfg.height, total, fps, fps / 60.0,
                js(std::filesystem::path(out).filename().string()).c_str());  // (file name only: no user paths in results)
    return 0;
}

int cmdAudio(const std::string& path) {
    MediaInfo mi = probeMedia(path);
    if (!mi.valid || mi.audio.empty()) {
        std::fprintf(stderr, "no audio\n");
        return 1;
    }
    std::string device = opt("--device", std::getenv("CUTLINE_AUDIO_DEVICE") ? std::getenv("CUTLINE_AUDIO_DEVICE") : "");
    if (device.empty()) {
        std::fprintf(stderr, "Refusing to play test audio on the default device. Pass --device <id> or set "
                             "CUTLINE_AUDIO_DEVICE. Output devices:\n");
        for (auto& [id, name] : listAudioDevices()) std::fprintf(stderr, "  %s  %s\n", id.c_str(), name.c_str());
        return 1;
    }
    int si = int(optd("--stream", 0));
    AudioReader ar;
    std::string err;
    if (!ar.open(path, mi.audio[size_t(si)].index, mi.origin, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    StereoRing ring(48000 / 2);
    std::atomic<uint64_t> underruns{0};
    std::atomic<bool> quit{false};
    std::thread producer([&] {
        std::vector<float> buf(480 * 2);
        int64_t pos = 0;
        while (!quit) {
            while (ring.space() >= 480) {
                ar.read(pos, buf.data(), 480);
                pos += 480;
                if (pos >= ar.lengthSamples()) pos = 0;
                ring.write(buf.data(), 480);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    while (ring.available() < 4800) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::string backend;
    auto out = createAudioOutput(
        [&](float* dst, int frames) {
            size_t got = ring.read(dst, size_t(frames));
            if (got < size_t(frames)) {
                std::fill(dst + got * 2, dst + size_t(frames) * 2, 0.f);
                ++underruns;
            }
        },
        backend, err, device);
    if (!out) {
        quit = true;
        producer.join();
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const double seconds = optd("--seconds", 10);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int64_t p0 = out->playedFrames();
    auto c0 = Clock::now();
    double maxDrift = 0;
    while (msSince(t0) < seconds * 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double wall = msSince(c0) / 1000.0;
        double played = double(out->playedFrames() - p0) / 48000.0;
        maxDrift = std::max(maxDrift, std::abs(wall - played) * 1000.0);
    }
    out->stop();
    quit = true;
    producer.join();
    std::printf("{\"test\":\"audio\",\"backend\":\"%s\",\"device\":\"%s\",\"seconds\":%.0f,\"buffer_ms\":%.1f,"
                "\"latency_ms\":%.1f,\"underruns\":%llu,\"device_glitches\":%llu,\"clock_drift_ms_max\":%.2f}\n",
                backend.c_str(), out->name().c_str(), seconds, out->bufferMs(), out->latencyMs(),
                (unsigned long long)underruns.load(), (unsigned long long)out->glitches(), maxDrift);
    return 0;
}

int cmdSeek(const std::string& path) {
    auto gpu = makeGpu();
    MediaInfo mi = probeMedia(path);
    if (!mi.valid) return 1;
    FrameReader rd;
    VideoDecoder::Options o;
    o.allowHw = !flag("--sw");
    std::string err;
    if (!rd.open(path, gpu, mi, o, err)) return 1;
    const int n = int(optd("--count", 60));
    std::mt19937 rng(42);
    std::uniform_int_distribution<int64_t> dist(0, mi.duration - kTicksPerSecond);
    std::vector<double> lat;
    int wrong = 0;
    for (int i = 0; i < n; ++i) {
        Tick t = dist(rng);
        rd.clearWindow();
        auto t0 = Clock::now();
        GpuFramePtr f = rd.get(t);
        gpu->context()->Flush();
        lat.push_back(msSince(t0));
        if (!f || f->pts > t + rd.frameDuration() / 2 || f->end.load() <= t - rd.frameDuration() / 2) ++wrong;
    }
    // scrub inside the decoded window (should be ~instant)
    std::vector<double> nearMs;
    Tick base = mi.duration / 2;
    rd.clearWindow();
    rd.get(base);
    for (int i = 0; i < 30; ++i) {
        auto t0 = Clock::now();
        Tick q = base - rd.frameDuration() * (i % 6);
        bool cached = rd.peek(q) != nullptr;
        rd.get(q);
        nearMs.push_back(msSince(t0));
        if (flag("--verbose")) std::fprintf(stderr, "near %d cached=%d %.3f ms\n", i, int(cached), nearMs.back());
    }
    std::printf("{\"test\":\"seek\",\"decoder\":\"%s\",\"count\":%d,\"ms_avg\":%.1f,\"ms_p95\":%.1f,\"ms_max\":%.1f,"
                "\"in_window_ms_avg\":%.3f,\"wrong_frames\":%d}\n",
                rd.describe().c_str(), n, std::accumulate(lat.begin(), lat.end(), 0.0) / lat.size(), pct(lat, 0.95),
                pct(lat, 1.0), std::accumulate(nearMs.begin(), nearMs.end(), 0.0) / nearMs.size(), wrong);
    return 0;
}

size_t privateBytes();

// Plays a project through the real PlaybackEngine in a window and reports what the HUD shows.
int cmdPlayback(const std::string& projPath) {
    Project p;
    std::string err;
    if (!loadProjectFile(projPath, p, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    resolveMediaPaths(p, projPath);
    const bool proxies = flag("--proxies");
    MediaCache cache;
    auto snap = std::make_shared<RenderSnapshot>();
    snap->seq = p.sequence;
    if (!flag("--audible")) snap->seq.masterVolumeDb = -96;  // benchmark silently
    snap->useProxies = proxies;
    int withProxy = 0;
    for (const auto& a : p.assets) {
        AssetRef r;
        r.id = a.id;
        r.path = a.path;
        r.info = a.info;
        r.offline = a.offline;
        if (proxies && cache.proxyReady(a)) {
            r.proxyPath = cache.proxyPath(a);
            r.proxyInfo = probeMedia(r.proxyPath);
            r.proxyInfo.origin = 0;
            ++withProxy;
        }
        snap->assets[a.id] = r;
    }
    addNestedSnapshots(*snap, p);
    PlaybackEngine eng;
    if (!eng.start(err)) {
        std::fprintf(stderr, "engine: %s\n", err.c_str());
        return 1;
    }
    int v = eng.addViewer();
    HWND hwnd = makeWindow(1280, 540);
    eng.attachWindow(v, hwnd, 1280, 540);
    eng.setContent(v, snap);
    const Tick start = secondsToTicks(optd("--start", 0));
    eng.seek(v, start);
    auto t0 = Clock::now();
    while (msSince(t0) < 1500) pump();  // let the first frames and decoders spin up
    const double seconds = optd("--seconds", 60);
    eng.play(v, 1.0);
    t0 = Clock::now();
    double minFps = 1e9;
    size_t rssPeak = privateBytes();
    int samples = 0;
    std::vector<double> fpsLog;
    auto lastSample = Clock::now();
    while (msSince(t0) < seconds * 1000 && eng.isPlaying()) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (msSince(lastSample) >= 1000) {
            lastSample = Clock::now();
            rssPeak = std::max(rssPeak, privateBytes());
            ViewerStats st = eng.stats(v);
            if (samples++ > 0) {  // skip the first (ramp-up) second
                minFps = std::min(minFps, st.fps);
                fpsLog.push_back(st.fps);
            }
        }
    }
    ViewerStats st = eng.stats(v);
    AudioStatus au = eng.audioStatus();
    eng.pause();
    double played = msSince(t0) / 1000.0;
    eng.detachWindow(v);
    eng.shutdown();
    DestroyWindow(hwnd);
    const double expected = played * snap->seq.settings.fps.toDouble();
    std::printf("{\"test\":\"playback\",\"project\":\"%s\",\"seconds\":%.1f,\"proxies\":%s,\"proxy_assets\":%d,"
                "\"frames_shown\":%llu,\"expected_frames\":%.0f,\"dropped\":%llu,\"dropped_pct\":%.3f,\"late\":%llu,"
                "\"fps_min\":%.0f,\"fps_avg\":%.1f,\"render_ms\":%.2f,\"gpu_ms\":%.3f,\"preview\":\"%s\","
                "\"decoders_gpu\":%d,\"decoders_cpu\":%d,\"audio\":\"%s\",\"audio_underruns\":%llu,\"mix_max_ms\":%.1f,\"rss_mb\":%.0f}\n",
                projPath.substr(projPath.find_last_of("/\\") + 1).c_str(), played, proxies ? "true" : "false", withProxy,
                (unsigned long long)st.framesShown, expected, (unsigned long long)st.dropped,
                expected > 0 ? 100.0 * double(st.dropped) / expected : 0.0, (unsigned long long)st.lateFrames,
                fpsLog.empty() ? 0 : minFps,
                fpsLog.empty() ? 0 : std::accumulate(fpsLog.begin(), fpsLog.end(), 0.0) / fpsLog.size(), st.renderMs,
                st.gpuMs, st.previewDivisor == 1 ? "Full" : st.previewDivisor == 2 ? "1/2" : "1/4", st.hwSources,
                st.swSources, au.backend.c_str(), (unsigned long long)au.underruns, au.mixMaxMs, double(rssPeak) / 1048576.0);
    return 0;
}

// JSON string escaping for paths and error messages in the result lines.
std::string js(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '\\' || ch == '"') o += '\\';
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
        o += ch;
    }
    return o;
}

size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

std::shared_ptr<RenderSnapshot> loadSnapshot(const std::string& projPath, bool proxies) {
    Project p;
    std::string err;
    if (!loadProjectFile(projPath, p, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return nullptr;
    }
    resolveMediaPaths(p, projPath);
    MediaCache cache;
    auto snap = std::make_shared<RenderSnapshot>();
    snap->seq = p.sequence;
    snap->useProxies = proxies;
    for (const auto& a : p.assets) {
        AssetRef r;
        r.id = a.id;
        r.path = a.path;
        r.info = a.info;
        r.offline = a.offline;
        if (proxies && cache.proxyReady(a)) {
            r.proxyPath = cache.proxyPath(a);
            r.proxyInfo = probeMedia(r.proxyPath);
            r.proxyInfo.origin = 0;
        }
        snap->assets[a.id] = r;
    }
    addNestedSnapshots(*snap, p);
    return snap;
}

// Full export of a project; samples RSS / VRAM to check they stay flat.
int cmdExport(const std::string& projPath) {
    auto snap = loadSnapshot(projPath, false);
    if (!snap) return 1;
    auto probeGpu = makeGpu();  // only for process VRAM queries
    ExportSettings es;
    es.path = opt("--out", (std::filesystem::temp_directory_path() / "mediabench_export.mp4").string());
    es.width = snap->seq.settings.width;
    es.height = snap->seq.settings.height;
    es.rangeEnd = snap->seq.duration();
    if (flag("--seconds")) es.rangeEnd = std::min(es.rangeEnd, secondsToTicks(optd("--seconds", 60)));
    es.video.codec = opt("--codec", "h264_nvenc");
    es.video.bitrate = 24'000'000;
    es.video.maxBitrate = 36'000'000;
    const size_t rss0 = privateBytes(), vram0 = probeGpu->vramUsage();
    ExportJob job(snap, es);
    auto t0 = Clock::now();
    job.start();
    size_t rssPeak = rss0, vramPeak = vram0;
    std::vector<std::pair<double, double>> rssSamples;  // (seconds, MB) after warm-up
    while (job.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        size_t r = privateBytes(), v = probeGpu->vramUsage();
        rssPeak = std::max(rssPeak, r);
        vramPeak = std::max(vramPeak, v);
        double t = msSince(t0) / 1000.0;
        if (t > 10) rssSamples.push_back({t, double(r) / 1048576.0});
    }
    job.wait();
    ExportStatus& st = job.status();
    double secs = msSince(t0) / 1000.0;
    double media = ticksToSeconds(es.rangeEnd);
    double slope = 0;  // least-squares RSS slope, MB per minute
    if (rssSamples.size() > 4) {
        double n = double(rssSamples.size()), sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (auto [x, y] : rssSamples) {
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        slope = (n * sxy - sx * sy) / std::max(1e-9, n * sxx - sx * sx) * 60.0;
    }
    std::printf("{\"test\":\"export\",\"project\":\"%s\",\"ok\":%s,\"error\":\"%s\",\"encoder\":\"%s\",\"frames\":%lld,"
                "\"media_s\":%.1f,\"wall_s\":%.1f,\"x_realtime\":%.2f,\"rss_start_mb\":%.0f,\"rss_peak_mb\":%.0f,"
                "\"rss_slope_mb_per_min\":%.2f,\"vram_start_mb\":%.0f,\"vram_peak_mb\":%.0f}\n",
                projPath.substr(projPath.find_last_of("/\\") + 1).c_str(), st.failed ? "false" : "true", js(st.error).c_str(),
                st.encoder.c_str(), (long long)st.framesDone.load(), media, secs, media / secs, double(rss0) / 1048576.0,
                double(rssPeak) / 1048576.0, slope, double(vram0) / 1048576.0, double(vramPeak) / 1048576.0);
    std::error_code ec;
    if (!flag("--keep")) std::filesystem::remove(es.path, ec);
    return st.failed ? 1 : 0;
}

// Builds a 720p all-intra proxy (the same code path the app uses) and reports its speed.
int cmdMakeProxy(const std::string& path) {
    Asset a;
    a.path = path;
    a.info = probeMedia(path);
    if (!a.info.valid) return 1;
    std::string out = opt("--out", path + ".proxy.mp4");
    JobToken tok;
    std::string err;
    auto t0 = Clock::now();
    bool ok = generateProxy(a, out, tok, err);
    double secs = msSince(t0) / 1000.0;
    std::error_code ec;
    double mb = ok ? double(std::filesystem::file_size(out, ec)) / 1048576.0 : 0;
    std::printf("{\"test\":\"proxy\",\"ok\":%s,\"error\":\"%s\",\"media_s\":%.1f,\"wall_s\":%.1f,\"x_realtime\":%.2f,"
                "\"size_mb\":%.0f,\"out\":\"%s\"}\n",
                ok ? "true" : "false", js(err).c_str(), ticksToSeconds(a.info.duration), secs,
                ticksToSeconds(a.info.duration) / std::max(0.001, secs), mb,
                js(std::filesystem::path(out).filename().string()).c_str());
    return ok ? 0 : 1;
}

// Soak: looped playback with bursts of random seeks; watches memory growth and stability.
int cmdSoak(const std::string& projPath) {
    auto snap = loadSnapshot(projPath, flag("--proxies"));
    if (!snap) return 1;
    snap->seq.masterVolumeDb = -96;
    const double minutes = optd("--minutes", 30);
    PlaybackEngine eng;
    std::string err;
    if (!eng.start(err)) return 1;
    int v = eng.addViewer();
    HWND hwnd = makeWindow(960, 400);
    eng.attachWindow(v, hwnd, 960, 400);
    eng.setContent(v, snap);
    auto gpu = eng.gpu();
    std::mt19937 rng(1);
    const Tick dur = snap->seq.duration();
    std::uniform_int_distribution<Tick> dist(0, std::max<Tick>(1, dur - kTicksPerSecond));
    auto t0 = Clock::now();
    auto lastSeekBurst = Clock::now();
    size_t rssStart = 0, rssMax = 0, vramStart = 0, vramMax = 0;
    uint64_t loops = 0, seeks = 0, droppedTotal = 0;
    eng.play(v, 1.0);
    bool sampledStart = false;
    std::vector<double> rssSeries;  // every 5 minutes: growth should flatten once caches are warm
    while (msSince(t0) < minutes * 60000) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!eng.isPlaying()) {  // reached the end: loop
            droppedTotal += eng.stats(v).dropped;
            ++loops;
            eng.seek(v, 0);
            eng.play(v, 1.0);
        }
        if (msSince(lastSeekBurst) > 20000) {
            droppedTotal += eng.stats(v).dropped;
            eng.pause();
            for (int i = 0; i < 20; ++i) {
                eng.seek(v, dist(rng));
                ++seeks;
                auto w = Clock::now();
                while (msSince(w) < 60) pump();
            }
            eng.play(v, 1.0);
            lastSeekBurst = Clock::now();
        }
        if (!sampledStart && msSince(t0) > 30000) {
            rssStart = privateBytes();
            vramStart = gpu->vramUsage();
            sampledStart = true;
        }
        if (sampledStart && msSince(t0) >= double(rssSeries.size() + 1) * 300000) rssSeries.push_back(double(privateBytes()) / 1048576.0);
        if (sampledStart) {
            rssMax = std::max(rssMax, privateBytes());
            vramMax = std::max(vramMax, gpu->vramUsage());
        }
    }
    eng.pause();
    AudioStatus au = eng.audioStatus();
    eng.detachWindow(v);
    eng.shutdown();
    DestroyWindow(hwnd);
    std::string series;
    for (double r : rssSeries) series += (series.empty() ? "" : ",") + std::to_string(int(r));
    std::printf("{\"test\":\"soak\",\"rss_every_5min_mb\":[%s],", series.c_str());
    std::printf("\"minutes\":%.1f,\"loops\":%llu,\"seeks\":%llu,\"dropped\":%llu,\"audio_underruns\":%llu,"
                "\"rss_after_30s_mb\":%.0f,\"rss_max_mb\":%.0f,\"vram_after_30s_mb\":%.0f,\"vram_max_mb\":%.0f}\n",
                minutes, (unsigned long long)loops, (unsigned long long)seeks, (unsigned long long)droppedTotal,
                (unsigned long long)au.underruns, double(rssStart) / 1048576.0, double(rssMax) / 1048576.0,
                double(vramStart) / 1048576.0, double(vramMax) / 1048576.0);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);  // text rendering (titles) needs a GUI application
    if (argc < 2) {
        std::fprintf(stderr, "usage: mediabench <decode|present|composite|encode|audio|seek> [file] [options]\n");
        return 1;
    }
    std::string cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) g_opts[a] = argv[++i];
            else g_opts[a] = "1";
        } else g_pos.push_back(a);
    }
    std::string file = g_pos.empty() ? "" : g_pos[0];
    if (cmd == "decode") return cmdDecode(file);
    if (cmd == "present") return cmdPresent(file);
    if (cmd == "composite") return cmdComposite(file);
    if (cmd == "scenes") {
        MediaInfo mi = probeMedia(file);
        std::vector<Tick> cuts;
        auto t0 = Clock::now();
        const bool ok = detectScenes(file, mi, 0, mi.duration, SceneOptions{}, cuts, {}, {});
        const double s = msSince(t0) / 1000.0;
        std::printf("{\"test\":\"scenes\",\"ok\":%s,\"cuts\":%zu,\"seconds\":%.1f,\"x_realtime\":%.1f}\n", ok ? "true" : "false", cuts.size(), s,
                    ticksToSeconds(mi.duration) / std::max(0.01, s));
        return ok ? 0 : 1;
    }
    if (cmd == "encode") return cmdEncode();
    if (cmd == "audio") return cmdAudio(file);
    if (cmd == "seek") return cmdSeek(file);
    if (cmd == "playback") return cmdPlayback(file);
    if (cmd == "export") return cmdExport(file);
    if (cmd == "soak") return cmdSoak(file);
    if (cmd == "makeproxy") return cmdMakeProxy(file);
    if (cmd == "makeprojects") return makeBenchProjects(opt("--media", CUTLINE_TESTMEDIA), file.empty() ? "." : file);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 1;
}
