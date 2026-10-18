// Benchmark project generator (used by `mediabench makeprojects` and bench.ps1).
#include "core/edits.h"
#include "core/project_io.h"
#include "media/probe.h"

#include <cstdio>
#include <string>

using namespace cut;

namespace {
constexpr Tick S = kTicksPerSecond;

Asset addAsset(Project& p, const std::string& path) {
    Asset a;
    a.id = p.newAssetId();
    a.path = path;
    a.name = path.substr(path.find_last_of("/\\") + 1);
    a.info = probeMedia(path);
    p.assets.push_back(a);
    return a;
}

bool save(Project& p, const std::string& path) {
    edit::normalize(p.sequence);
    std::string err;
    if (!saveProjectFile(p, path, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    std::printf("wrote %s\n", path.c_str());
    return true;
}
} // namespace

int makeBenchProjects(const std::string& media, const std::string& outDir) {
    // 1/2) one video + two audio tracks with colour correction (1440p and ultrawide)
    for (auto [file, name, w] : {std::tuple{"main_1440p60_h264.mkv", "bench_1440p.cutproj", 2560},
                                 std::tuple{"uw_3440x1440p60_hevc.mkv", "bench_ultrawide.cutproj", 3440}}) {
        Project p;
        p.name = name;
        p.sequence = makeDefaultSequence({w, 1440, {60, 1}, 48000});
        Asset a = addAsset(p, media + "/" + file);
        if (!a.info.valid) {
            std::fprintf(stderr, "missing %s\n", file);
            return 1;
        }
        Tick dur = std::min<Tick>(a.info.duration, 300 * S);
        auto clips = edit::clipsForAsset(p.sequence, a, 0, 0, dur, 0, 0);
        auto ids = edit::overwriteClips(p.sequence, clips);
        Effect cc = makeEffect(fx::Color);
        cc.params["contrast"].value = 15;
        cc.params["saturation"].value = 120;
        p.sequence.clip(ids[0])->effects.push_back(cc);
        // second audio track: the uw file has one stream, duplicate it so there are 2 audio tracks
        if (a.info.audio.size() == 1) {
            Clip extra = *p.sequence.clip(ids[1]);
            extra.id = 0;
            extra.trackId = p.sequence.trackAt(TrackKind::Audio, 1)->id;
            edit::overwriteClips(p.sequence, {extra});
        }
        if (!save(p, outDir + "/" + name)) return 1;
        if (std::string(name) == "bench_1440p.cutproj") {
            // three speed ramps (25 %, eased) in the first 40 s: many short segments, each its own decoder
            for (Tick at : {30 * S, 20 * S, 10 * S}) {
                Id v = 0;
                for (const auto& x : p.sequence.clips)
                    if (p.sequence.track(x.trackId)->kind == TrackKind::Video && x.contains(at)) v = x.id;
                if (v) edit::speedRamp(p.sequence, p.assets, v, at, 0.25, S, S / 2);
            }
            p.name = "bench_ramp";
            if (!save(p, outDir + "/bench_ramp.cutproj")) return 1;
        }
    }
    // 3) three stacked video tracks: full frame + PiP + title
    {
        Project p;
        p.name = "bench_3layers";
        p.sequence = makeDefaultSequence({2560, 1440, {60, 1}, 48000});
        Asset a = addAsset(p, media + "/main_1440p60_h264.mkv");
        Asset u = addAsset(p, media + "/uw_3440x1440p60_hevc.mkv");
        Tick dur = std::min<Tick>(u.info.duration, 120 * S);
        edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, a, 0, 0, dur, 0, 0));
        auto pip = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, u, 0, 0, dur, 1, 2, true, false));
        Clip* c = p.sequence.clip(pip[0]);
        c->motion.params["scale"].value = 35;
        c->motion.params["posX"].value = 700;
        c->motion.params["posY"].value = -380;
        Effect cc = makeEffect(fx::Color);
        cc.params["saturation"].value = 140;
        c->effects.push_back(cc);
        Clip t = makeTextClip(p.sequence.trackAt(TrackKind::Video, 2)->id, 0, dur, "Benchmark Title");
        t.motion.params["posY"].value = 520;
        t.textStyle.box = true;
        edit::overwriteClips(p.sequence, {t});
        if (!save(p, outDir + "/bench_3layers.cutproj")) return 1;
        // the same three layers nested (the nest is the only clip of the main sequence)
        std::vector<Id> all;
        for (const auto& x : p.sequence.clips) all.push_back(x.id);
        edit::nestClips(p, all, "Nest");
        p.name = "bench_nested";
        if (!save(p, outDir + "/bench_nested.cutproj")) return 1;
    }
    // 4) 500 clips across 6 tracks (timeline stress)
    {
        Project p;
        p.name = "bench_500clips";
        p.sequence = makeDefaultSequence({2560, 1440, {60, 1}, 48000});
        Asset a = addAsset(p, media + "/main_1440p60_h264.mkv");
        Tick at = 0;
        for (int i = 0; i < 250; ++i) {
            Tick len = S / 2 + (i % 7) * S / 4;
            Tick src = (i * 37 % 500) * S;
            edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, a, at, src, src + len, i % 2, 0));
            at += len;
        }
        std::printf("500-clip sequence: %zu clips\n", p.sequence.clips.size());
        if (!save(p, outDir + "/bench_500clips.cutproj")) return 1;
    }
    return 0;
}
