// e2e: builds a sequence programmatically (razor, ripple delete, title, cross dissolve, colour
// correction, ultrawide fill, 50 % speed, freeze frame, captions from SRT, audio fades, A/V sync
// clip), exports it with NVENC and with the libx264 fallback, and validates the files:
// resolution, fps, exact frame count and duration, 48 kHz audio, clean decode, A/V sync
// (flash frame vs beep onset within +-1 frame) and sampled frames against goldens.
//   e2e [--update] [--keep]
#include "audio/loudness.h"
#include "core/edits.h"
#include "core/srt.h"
#include "engine/snapshot.h"
#include "export/export_job.h"
#include "export/gif_export.h"
#include "media/ff.h"
#include "media/frame_reader.h"
#include "media/probe.h"

#include <QDir>
#include <QGuiApplication>
#include <QImage>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace cut;

namespace {
constexpr Tick S = kTicksPerSecond;
std::string g_media = CUTLINE_TESTMEDIA;
std::string g_golden = CUTLINE_GOLDEN;
int g_fail = 0, g_pass = 0;
bool g_update = false;

void report(bool ok, const std::string& name, const std::string& detail) {
    std::printf("%s %-46s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

Asset load(Project& p, const char* file) {
    Asset a;
    a.id = p.newAssetId();
    a.path = g_media + "/" + file;
    a.name = file;
    a.info = probeMedia(a.path);
    p.assets.push_back(a);
    return a;
}

std::shared_ptr<RenderSnapshot> buildSequence(Tick& total) {
    Project p;
    p.sequence = makeDefaultSequence({2560, 1440, {60, 1}, 48000});
    Sequence& s = p.sequence;
    Asset mainA = load(p, "main_1440p60_h264.mkv");
    Asset uwA = load(p, "uw_3440x1440p60_hevc.mkv");
    Asset syncA = load(p, "sync_1080p60.mp4");
    // V1: main [0,8) src 10..18, razor at 2 and 4, ripple delete [2,4) -> [0,6)
    auto clips = edit::clipsForAsset(s, mainA, 0, 10 * S, 18 * S, 0, 0);
    Id v = edit::overwriteClips(s, clips).front();
    for (const Clip& c : s.clips)
        if (c.trackId == s.trackAt(TrackKind::Audio, 0)->id && c.linkId == s.clip(v)->linkId) {
            Clip* a = s.clip(c.id);
            a->fadeIn = S / 2;
        }
    auto r1 = edit::razor(s, v, 2 * S);
    auto r2 = edit::razor(s, r1[0], 4 * S);
    edit::rippleDelete(s, s.linkedClips(r1[0]));
    edit::normalize(s);
    // V1: ultrawide [6,10) src 5..9 fill + colour correction, cross dissolve at 6 s
    clips = edit::clipsForAsset(s, uwA, 6 * S, 5 * S, 9 * S, 0, 0);
    Id uw = edit::overwriteClips(s, clips).front();
    s.clip(uw)->fit = FitMode::Fill;
    Effect cc = makeEffect(fx::Color);
    cc.params["saturation"].value = 160;
    cc.params["contrast"].value = 20;
    s.clip(uw)->effects.push_back(cc);
    edit::addTransition(s, 0, r2[0], uw, TransitionType::CrossDissolve, S, TransitionAlign::Center);
    // V1: main at 50 % speed [10,14) src 30..32
    clips = edit::clipsForAsset(s, mainA, 10 * S, 30 * S, 32 * S, 0, 0);
    Id slow = edit::overwriteClips(s, clips).front();
    edit::setSpeed(s, p.assets, slow, 0.5, false, false, true);
    // V1: freeze frame of src 40 s on [14,16)
    clips = edit::clipsForAsset(s, mainA, 14 * S, 40 * S, 42 * S, 0, 0, true, false);
    Id frz = edit::overwriteClips(s, clips).front();
    edit::addFrameHold(s, frz, 14 * S);
    // V1 + A1: A/V sync clip [16,22)
    clips = edit::clipsForAsset(s, syncA, 16 * S, 0, 6 * S, 0, 0);
    edit::overwriteClips(s, clips);
    // V2: title [1,4) with fades
    Clip t = makeTextClip(s.trackAt(TrackKind::Video, 1)->id, 1 * S, 3 * S, "Cutline E2E");
    t.fadeIn = S / 2;
    t.fadeOut = S / 2;
    t.textStyle.font = "Arial";
    t.motion.params["posY"].value = -400;
    edit::overwriteClips(s, {t});
    // captions from SRT
    std::vector<SrtEntry> srt;
    std::string err;
    parseSrt("1\n00:00:00,500 --> 00:00:02,000\nFirst caption\n\n2\n00:00:03,000 --> 00:00:05,000\nSecond caption\n", srt, err);
    for (const auto& e : srt) edit::addCaption(s, e.start, e.end, e.text);
    edit::normalize(s);
    total = s.duration();
    auto snap = std::make_shared<RenderSnapshot>();
    snap->seq = s;
    for (const auto& a : p.assets) {
        AssetRef r;
        r.id = a.id;
        r.path = a.path;
        r.info = a.info;
        // Export must never use proxies: give every asset a bogus "proxy" (the colour chart).
        // If export used it, the sampled frames would not match the goldens.
        r.proxyPath = g_media + "/color_full_709.mp4";
        r.proxyInfo = probeMedia(r.proxyPath);
        snap->assets[a.id] = r;
    }
    snap->useProxies = true;
    return snap;
}

struct Probe {
    int w = 0, h = 0;
    Rational fps;
    int64_t frames = 0, decodeErrors = 0;
    double duration = 0;
    int audioRate = 0, audioChannels = 0;
    std::vector<double> flashTimes;  // frames whose mean luma is high
    std::vector<double> beepTimes;   // 1 kHz onsets
    std::vector<float> stereo;       // decoded audio, interleaved L/R
    std::vector<std::pair<double, QImage>> samples;
};

Probe analyze(const std::string& path, const std::vector<double>& sampleAt, double syncFrom) {
    Probe r;
    std::string err;
    ff::FormatPtr fmt = ff::openInput(path, err);
    if (!fmt) return r;
    int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vi < 0) return r;
    AVStream* vs = fmt->streams[vi];
    r.w = vs->codecpar->width;
    r.h = vs->codecpar->height;
    r.fps = ff::rat(vs->avg_frame_rate);
    r.duration = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0;
    ff::CodecPtr vc(avcodec_alloc_context3(avcodec_find_decoder(vs->codecpar->codec_id)));
    avcodec_parameters_to_context(vc.get(), vs->codecpar);
    vc->thread_count = 0;
    vc->err_recognition = AV_EF_EXPLODE;
    avcodec_open2(vc.get(), vc->codec, nullptr);
    ff::CodecPtr ac;
    ff::SwrPtr swr;
    if (ai >= 0) {
        AVStream* as = fmt->streams[ai];
        r.audioRate = as->codecpar->sample_rate;
        r.audioChannels = as->codecpar->ch_layout.nb_channels;
        ac.reset(avcodec_alloc_context3(avcodec_find_decoder(as->codecpar->codec_id)));
        avcodec_parameters_to_context(ac.get(), as->codecpar);
        avcodec_open2(ac.get(), ac->codec, nullptr);
    }
    std::vector<float> mono;  // whole audio track, mono
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr f = ff::makeFrame();
    ff::SwsPtr sws;
    size_t nextSample = 0;
    auto handleVideo = [&] {
        while (true) {
            int rr = avcodec_receive_frame(vc.get(), f.get());
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
            if (rr < 0) {
                ++r.decodeErrors;
                break;
            }
            double t = double(f->pts) * av_q2d(vs->time_base);
            ++r.frames;
            // mean luma of the frame (Y plane) for flash detection
            if (t >= syncFrom) {
                double sum = 0;
                const int stepY = 16;
                int n = 0;
                for (int y = 200; y < f->height; y += stepY)
                    for (int x = 0; x < f->width; x += 16, ++n) sum += f->data[0][size_t(y) * f->linesize[0] + x];
                if (sum / n > 128) r.flashTimes.push_back(t);
            }
            if (nextSample < sampleAt.size() && std::abs(t - sampleAt[nextSample]) < 0.5 / 60) {
                if (!sws)
                    sws.reset(sws_getContext(f->width, f->height, AVPixelFormat(f->format), 1280, 720, AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr));
                QImage img(1280, 720, QImage::Format_RGB32);
                uint8_t* d[4] = {img.bits(), nullptr, nullptr, nullptr};
                int ls[4] = {int(img.bytesPerLine()), 0, 0, 0};
                sws_scale(sws.get(), f->data, f->linesize, 0, f->height, d, ls);
                r.samples.push_back({t, img});
                ++nextSample;
            }
            av_frame_unref(f.get());
        }
    };
    auto handleAudio = [&] {
        while (avcodec_receive_frame(ac.get(), f.get()) == 0) {
            for (int i = 0; i < f->nb_samples; ++i) {
                float v = 0;
                for (int c = 0; c < f->ch_layout.nb_channels; ++c) v += reinterpret_cast<float*>(f->extended_data[c])[i];
                mono.push_back(v / f->ch_layout.nb_channels);
                if (f->ch_layout.nb_channels == 2 && f->format == AV_SAMPLE_FMT_FLTP) {
                    r.stereo.push_back(reinterpret_cast<float*>(f->extended_data[0])[i]);
                    r.stereo.push_back(reinterpret_cast<float*>(f->extended_data[1])[i]);
                }
            }
            av_frame_unref(f.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == vi) {
            if (avcodec_send_packet(vc.get(), pkt.get()) < 0) ++r.decodeErrors;
            handleVideo();
        } else if (pkt->stream_index == ai && ac) {
            avcodec_send_packet(ac.get(), pkt.get());
            handleAudio();
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(vc.get(), nullptr);
    handleVideo();
    if (ac) {
        avcodec_send_packet(ac.get(), nullptr);
        handleAudio();
    }
    // 1 kHz beep onsets: Goertzel energy in 1 ms windows (other tones are 330-880 Hz)
    if (r.audioRate > 0) {
        const int win = r.audioRate / 1000;
        const double k = 2 * std::cos(2 * 3.14159265358979 * 1000.0 / r.audioRate);
        bool on = false;
        for (size_t s = size_t(syncFrom * r.audioRate); s + size_t(win) * 4 < mono.size(); s += size_t(win)) {
            double q1 = 0, q2 = 0;
            for (int i = 0; i < win * 4; ++i) {
                double q0 = k * q1 - q2 + mono[s + size_t(i)];
                q2 = q1;
                q1 = q0;
            }
            double power = q1 * q1 + q2 * q2 - k * q1 * q2;
            double norm = power / (win * 4.0 * win * 4.0);
            if (!on && norm > 0.01) {
                r.beepTimes.push_back(double(s) / r.audioRate);
                on = true;
            } else if (on && norm < 0.001) on = false;
        }
    }
    return r;
}

void goldenCompare(const std::string& name, const QImage& img) {
    std::string path = g_golden + "/" + name + ".png";
    if (g_update || !std::filesystem::exists(path)) {
        img.save(QString::fromStdString(path));
        report(true, name, "(golden written)");
        return;
    }
    QImage ref = QImage(QString::fromStdString(path)).convertToFormat(QImage::Format_RGB32);
    if (ref.size() != img.size()) {
        report(false, name, "size mismatch");
        return;
    }
    int64_t bad = 0;
    double sum = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            QRgb a = img.pixel(x, y), b = ref.pixel(x, y);
            int d = std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
            sum += d;
            if (d > 24) ++bad;
        }
    double frac = double(bad) / (img.width() * img.height());
    char buf[96];
    std::snprintf(buf, sizeof(buf), "bad=%.3f%% mean=%.2f", frac * 100, sum / (img.width() * img.height()));
    report(frac < 0.01 && sum / (img.width() * img.height()) < 3.0, name, buf);
}

// Loudness normalization: the export pre-pass measures the mix and applies one gain; the file's
// measured loudness must equal measured + gain (and the gain must be positive for this quiet mix).
void loudnessExport(const std::shared_ptr<RenderSnapshot>& snap, const std::string& out, bool keep) {
    ExportSettings es;
    es.path = out;
    es.width = 640;
    es.height = 360;
    es.rangeStart = 0;
    es.rangeEnd = 6 * S;
    es.video.codec = "libx264";
    es.video.bitrate = 2'000'000;
    es.video.maxBitrate = 4'000'000;
    es.normalizeLufs = -14;
    ExportJob job(snap, es);
    job.start();
    job.wait();
    ExportStatus& st = job.status();
    if (st.failed) {
        report(false, "loudness: export", st.error);
        return;
    }
    Probe p = analyze(out, {}, 1e9);
    LoudnessMeter m;
    m.push(p.stereo.data(), int(p.stereo.size() / 2));
    const double got = m.integrated(), expect = st.measuredLufs + st.appliedGainDb;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "mix %.1f LUFS, gain %+.1f dB -> file %.1f LUFS (expected %.1f, peak %.1f dBFS)",
                  st.measuredLufs, st.appliedGainDb, got, expect, 20 * std::log10(std::max(1e-9, m.samplePeak())));
    report(std::abs(got - expect) < 0.7 && st.appliedGainDb > 3 && m.samplePeak() < 0.95, "loudness: normalized export", buf);
    if (!keep) std::filesystem::remove(out);
}
// Audio only: the same 6 s mix as WAV / FLAC / MP3 / M4A - no video stream, right length, same loudness
void audioOnlyExport(const std::shared_ptr<RenderSnapshot>& snap, const std::string& dir, bool keep) {
    struct Fmt {
        const char* ext;
        const char* codec;
        int bitrate;
    };
    double ref = 0;
    for (const Fmt f : {Fmt{"wav", "pcm_s16le", 0}, Fmt{"flac", "flac", 0}, Fmt{"mp3", "libmp3lame", 320000}, Fmt{"m4a", "aac", 256000}}) {
        ExportSettings es;
        es.path = dir + "/e2e_audio." + f.ext;
        es.withVideo = false;
        es.audioCodec = f.codec;
        es.audioBitrate = f.bitrate;
        es.rangeStart = 0;
        es.rangeEnd = 6 * S;
        ExportJob job(snap, es);
        job.start();
        job.wait();
        ExportStatus& st = job.status();
        if (st.failed) {
            report(false, std::string("audio only: ") + f.ext, st.error);
            continue;
        }
        MediaInfo mi = probeMedia(es.path);
        std::vector<float> pcm;  // (the video-led analyze() needs a picture)
        {
            AudioReader rd;  // closed before the file is removed
            std::string rerr;
            if (mi.valid && !mi.audio.empty() && rd.open(es.path, mi.audio[0].index, mi.origin, rerr)) {
                pcm.resize(size_t(rd.lengthSamples()) * 2);
                rd.read(0, pcm.data(), int(rd.lengthSamples()));
            }
        }
        LoudnessMeter m;
        m.push(pcm.data(), int(pcm.size() / 2));
        const double lufs = m.integrated(), secs = double(pcm.size() / 2) / 48000.0;
        if (ref == 0) ref = lufs;  // the WAV is the reference
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%.3f s, %.2f LUFS (wav %.2f), video %d", secs, lufs, ref, int(mi.hasVideo));
        report(mi.valid && !mi.hasVideo && !mi.audio.empty() && std::abs(secs - 6.0) < 0.06 && std::abs(lufs - ref) < 0.3,
               std::string("audio only: ") + f.ext, buf);
        std::error_code ec;
        if (!keep) std::filesystem::remove(es.path, ec);
    }
}
// Animated GIF: 2 s at 10 fps, 320 wide -> 20 paletted frames of 320x180 that decode cleanly.
void gifExport(const std::shared_ptr<RenderSnapshot>& snap, const std::string& out, bool keep) {
    GifSettings gs;
    gs.path = out;
    gs.width = 320;
    gs.fps = 10;
    gs.from = 0;
    gs.to = 2 * S;
    std::atomic<double> prog{0};
    std::atomic<bool> cancel{false};
    std::string err;
    const bool ok = exportGif(snap, gs, prog, cancel, err);
    if (!ok) {
        report(false, "gif: export", err);
        return;
    }
    MediaInfo mi = probeMedia(out);
    Probe p = analyze(out, {}, 1e9);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%dx%d, %lld frames, %lld decode errors", mi.video.width, mi.video.height, (long long)p.frames,
                  (long long)p.decodeErrors);
    report(mi.valid && mi.video.width == 320 && mi.video.height == 180 && p.frames == 20 && p.decodeErrors == 0, "gif: 2 s at 10 fps", buf);
    if (!keep) std::filesystem::remove(out);
}
void runExport(const std::shared_ptr<RenderSnapshot>& snap, Tick total, const std::string& codec, const std::string& out,
               bool keep) {
    ExportSettings es;
    es.path = out;
    es.width = 2560;
    es.height = 1440;
    es.rangeStart = 0;
    es.rangeEnd = total;
    es.video.codec = codec;
    es.video.bitrate = 30'000'000;
    es.video.maxBitrate = 45'000'000;
    es.burnCaptions = true;
    es.sidecarSrt = true;
    es.allowFallback = false;
    ExportJob job(snap, es);
    auto t0 = std::chrono::steady_clock::now();
    job.start();
    job.wait();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ExportStatus& st = job.status();
    if (st.failed) {
        report(false, codec + ": export", st.error);
        return;
    }
    const int64_t expectedFrames = ticksToFrame(total, {60, 1});
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s, %.1f s for %.1f s of video (%.2fx realtime)", st.encoder.c_str(), secs,
                  ticksToSeconds(total), ticksToSeconds(total) / secs);
    report(true, codec + ": export", buf);
    const double syncFrom = 16.0;
    std::vector<double> samples = {1.5, 6.0, 8.0, 12.0, 15.0, 17.0};
    Probe p = analyze(out, samples, syncFrom);
    report(p.w == 2560 && p.h == 1440, codec + ": resolution", std::to_string(p.w) + "x" + std::to_string(p.h));
    report(p.fps == Rational{60, 1}, codec + ": fps", std::to_string(p.fps.num) + "/" + std::to_string(p.fps.den));
    report(p.frames == expectedFrames, codec + ": exact frame count",
           std::to_string(p.frames) + " (expected " + std::to_string(expectedFrames) + ")");
    std::snprintf(buf, sizeof(buf), "%.4f s (expected %.4f)", p.duration, ticksToSeconds(total));
    report(std::abs(p.duration - ticksToSeconds(total)) < 0.03, codec + ": duration", buf);
    report(p.audioRate == 48000 && p.audioChannels == 2, codec + ": audio 48 kHz stereo",
           std::to_string(p.audioRate) + " Hz x" + std::to_string(p.audioChannels));
    report(p.decodeErrors == 0, codec + ": clean decode", std::to_string(p.decodeErrors) + " errors");
    // A/V sync: each flash (one frame, every second) vs the nearest beep onset
    double worst = 0;
    int matched = 0;
    for (double ft : p.flashTimes) {
        double best = 1e9;
        for (double bt : p.beepTimes) best = std::min(best, std::abs(bt - ft));
        if (best < 0.5) {
            ++matched;
            worst = std::max(worst, best);
        }
    }
    std::snprintf(buf, sizeof(buf), "%d flashes / %d beeps matched, worst offset %.1f ms (limit 16.7)", matched,
                  int(p.beepTimes.size()), worst * 1000);
    report(matched >= 5 && worst <= 1.0 / 60 + 0.002, codec + ": A/V sync", buf);
    std::string srt = out.substr(0, out.rfind('.')) + ".srt";
    report(std::filesystem::exists(srt), codec + ": sidecar .srt written", "");
    for (auto& [t, img] : p.samples) {
        char n[64];
        std::snprintf(n, sizeof(n), "e2e_%s_%05.2f", codec == "libx264" ? "x264" : "nvenc", t);
        goldenCompare(n, img);
    }
    if (!keep) {
        std::error_code ec;
        std::filesystem::remove(out, ec);
        std::filesystem::remove(srt, ec);
    }
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    QGuiApplication app(argc, argv);
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--update") g_update = true;
        if (a == "--keep") keep = true;
    }
    if (!std::filesystem::exists(g_media + "/sync_1080p60.mp4")) {
        std::printf("testmedia missing: run scripts\\gen-testmedia.ps1\n");
        return 2;
    }
    Tick total = 0;
    auto snap = buildSequence(total);
    report(total == 22 * S, "sequence built (22 s)", std::to_string(ticksToSeconds(total)) + " s");
    std::string dir = (std::filesystem::temp_directory_path() / "cutline_e2e").string();
    std::filesystem::create_directories(dir);
    runExport(snap, total, "h264_nvenc", dir + "/e2e_nvenc.mp4", keep);
    runExport(snap, total, "libx264", dir + "/e2e_x264.mp4", keep);
    runExport(snap, total, "hevc_nvenc", dir + "/e2e_hevc.mp4", keep);
    runExport(snap, total, "av1_nvenc", dir + "/e2e_av1.mp4", keep);
    loudnessExport(snap, dir + "/e2e_loudness.mp4", keep);
    gifExport(snap, dir + "/e2e.gif", keep);
    audioOnlyExport(snap, dir, keep);
    std::printf("\ne2e: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
