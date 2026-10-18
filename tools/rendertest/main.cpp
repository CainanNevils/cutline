// rendertest: GPU render correctness.
//  - colour: limited/full range and BT.601/BT.709 charts decode to known RGB values,
//  - golden frames (tolerance-based) for transform, opacity, 3-layer stack, effects,
//    transitions at 0/50/100 %, titles (stroke/shadow/box), captions, NV12 conversion.
//   rendertest [--update] [--out dir]   (--update rewrites tests/golden/*.png)
#include "core/edits.h"
#include "engine/frame_renderer.h"
#include "engine/sync_provider.h"
#include "media/probe.h"
#include "render/compositor.h"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace cut;

namespace {
std::string g_media = CUTLINE_TESTMEDIA;
std::string g_golden = CUTLINE_GOLDEN;
std::string g_outDir;
bool g_update = false;
int g_fail = 0, g_pass = 0;

std::shared_ptr<GpuDevice> g_gpu;
Compositor* g_comp = nullptr;

void report(bool ok, const std::string& name, const std::string& detail) {
    std::printf("%s %-40s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    (ok ? g_pass : g_fail)++;
}

QImage readCanvas(const GpuTexture& canvas) {
    std::vector<uint8_t> data;
    int pitch = 0;
    g_comp->readback(canvas, data, pitch);
    QImage img(canvas.width, canvas.height, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < canvas.height; ++y) std::memcpy(img.scanLine(y), data.data() + size_t(y) * pitch, size_t(canvas.width) * 4);
    // composite over black like the monitor / export
    QImage out(canvas.width, canvas.height, QImage::Format_RGB32);
    out.fill(Qt::black);
    for (int y = 0; y < img.height(); ++y) {
        const uint8_t* s = img.constScanLine(y);
        QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < img.width(); ++x) d[x] = qRgb(s[x * 4], s[x * 4 + 1], s[x * 4 + 2]);
    }
    return out;
}

void golden(const std::string& name, const QImage& img, int tol = 8, double maxBadFrac = 0.002) {
    std::string path = g_golden + "/" + name + ".png";
    if (!g_outDir.empty()) img.save(QString::fromStdString(g_outDir + "/" + name + ".png"));
    if (g_update || !std::filesystem::exists(path)) {
        img.save(QString::fromStdString(path));
        report(true, name, g_update ? "(golden updated)" : "(golden created)");
        return;
    }
    QImage ref(QString::fromStdString(path));
    ref = ref.convertToFormat(QImage::Format_RGB32);
    if (ref.size() != img.size()) {
        report(false, name, "size mismatch");
        return;
    }
    int64_t bad = 0;
    double sum = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* a = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const QRgb* b = reinterpret_cast<const QRgb*>(ref.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            int d = std::max({std::abs(qRed(a[x]) - qRed(b[x])), std::abs(qGreen(a[x]) - qGreen(b[x])),
                              std::abs(qBlue(a[x]) - qBlue(b[x]))});
            sum += d;
            if (d > tol) ++bad;
        }
    }
    double frac = double(bad) / double(img.width() * img.height());
    char buf[96];
    std::snprintf(buf, sizeof(buf), "bad=%.4f%% mean=%.2f", frac * 100, sum / (img.width() * img.height()));
    report(frac <= maxBadFrac, name, buf);
}

AssetRef asset(Id id, const std::string& file) {
    AssetRef a;
    a.id = id;
    a.path = g_media + "/" + file;
    a.info = probeMedia(a.path);
    return a;
}

struct Scene {
    RenderSnapshot snap;
    Id v1, v2, v3;
    Scene(int w = 1280, int h = 720) {
        snap.seq = makeDefaultSequence({w, h, {30, 1}, 48000});
        v1 = snap.seq.trackAt(TrackKind::Video, 0)->id;
        v2 = snap.seq.trackAt(TrackKind::Video, 1)->id;
        v3 = snap.seq.trackAt(TrackKind::Video, 2)->id;
    }
    Clip& add(Id track, Id assetId, Tick start = 0, Tick dur = kTicksPerSecond * 4) {
        const AssetRef* a = snap.asset(assetId);
        Asset tmp;
        tmp.id = assetId;
        tmp.info = a->info;
        tmp.name = "x";
        Clip c = makeVideoClip(tmp, track, start, 0, dur);
        c.id = snap.seq.newId();
        snap.seq.clips.push_back(c);
        return snap.seq.clips.back();
    }
    Clip& addText(Id track, const std::string& text, const TextStyle& st) {
        Clip c = makeTextClip(track, 0, kTicksPerSecond * 4, text);
        c.textStyle = st;
        c.id = snap.seq.newId();
        snap.seq.clips.push_back(c);
        return snap.seq.clips.back();
    }
    QImage render(Tick t = 0) {
        SyncFrameProvider prov(g_gpu, true);
        FrameRenderer r(*g_comp);
        auto canvas = g_comp->makeCanvas(snap.seq.settings.width, snap.seq.settings.height, false);
        r.render(snap, t, *canvas, prov, true);
        return readCanvas(*canvas);
    }
};

void colorChart(const char* file, const char* name) {
    Scene s;
    s.snap.assets[1] = asset(1, file);
    if (!s.snap.assets[1].info.valid) {
        report(false, name, "missing test media");
        return;
    }
    s.add(s.v1, 1);
    QImage img = s.render();
    struct P {
        int x, y;
        int r, g, b;
    };
    // patch centres of the generated chart (see gen-testmedia.ps1)
    const P patches[] = {{160, 180, 255, 255, 255}, {480, 180, 128, 128, 128}, {800, 180, 0xC0, 0x30, 0x20},
                         {1120, 180, 0x20, 0x60, 0xC0}, {320, 540, 0x20, 0xA0, 0x40}, {960, 540, 0, 0, 0}};
    int worst = 0;
    for (const auto& p : patches) {
        QRgb c = img.pixel(p.x, p.y);
        worst = std::max({worst, std::abs(qRed(c) - p.r), std::abs(qGreen(c) - p.g), std::abs(qBlue(c) - p.b)});
    }
    report(worst <= 5, name, "max channel error " + std::to_string(worst) + " (<=5)");
}

TextStyle titleStyle() {
    TextStyle st;
    st.font = "Arial";
    st.size = 90;
    st.stroke = true;
    st.strokeWidth = 6;
    st.strokeColor = 0xFF1060E0;
    st.shadow = true;
    st.shadowX = 8;
    st.shadowY = 8;
    st.shadowBlur = 10;
    st.box = true;
    st.boxColor = 0xFF202020;
    st.boxOpacity = 70;
    return st;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");  // offscreen has no font database of its own
    QGuiApplication app(argc, argv);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--update") g_update = true;
        else if (a == "--out" && i + 1 < argc) g_outDir = argv[++i];
    }
    if (!g_outDir.empty()) QDir().mkpath(QString::fromStdString(g_outDir));
    QDir().mkpath(QString::fromStdString(g_golden));
    std::string err;
    g_gpu = GpuDevice::create(false, err);
    if (!g_gpu) {
        std::printf("no GPU: %s\n", err.c_str());
        return 2;
    }
    Compositor comp;
    if (!comp.init(g_gpu, err)) return 2;
    g_comp = &comp;

    // --- colour correctness ---------------------------------------------------------------
    colorChart("color_limited_709.mp4", "color: limited range BT.709");
    colorChart("color_full_709.mp4", "color: full range BT.709");
    colorChart("color_limited_601.mp4", "color: limited range BT.601");

    // --- transform / opacity / stack ----------------------------------------------------------
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        c.motion.params["scale"].value = 50;
        c.motion.params["rotation"].value = 15;
        c.motion.params["posX"].value = 200;
        c.motion.params["posY"].value = -80;
        golden("transform", s.render());
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        c.opacity.params["opacity"].value = 50;
        golden("opacity", s.render());
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        s.snap.assets[2] = asset(2, "color_full_709.mp4");
        s.add(s.v1, 2);
        Clip& b = s.add(s.v2, 1);
        b.motion.params["scale"].value = 40;
        b.motion.params["posX"].value = -300;
        b.motion.params["posY"].value = -150;
        Clip& c = s.add(s.v3, 1);
        c.motion.params["scale"].value = 30;
        c.motion.params["posX"].value = 350;
        c.motion.params["posY"].value = 180;
        c.opacity.params["opacity"].value = 70;
        c.opacity.params["blend"].value = double(BlendMode::Screen);
        golden("stack_3_layers", s.render());
    }
    {
        Scene s(2560, 1440);
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        c.fit = FitMode::Fill;
        golden("fill_ultrawide_reframe", s.render(), 8);
    }
    // --- effects --------------------------------------------------------------------------------
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        Effect crop = makeEffect(fx::Crop);
        crop.params["left"].value = 20;
        crop.params["bottom"].value = 30;
        c.effects.push_back(crop);
        golden("effect_crop", s.render());
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "color_full_709.mp4");
        Clip& c = s.add(s.v1, 1);
        Effect cc = makeEffect(fx::Color);
        cc.params["exposure"].value = 0.5;
        cc.params["contrast"].value = 30;
        cc.params["saturation"].value = 140;
        cc.params["temperature"].value = 40;
        c.effects.push_back(cc);
        golden("effect_color", s.render());
    }
    {
        // LUT that inverts colours
        std::string lutPath = (std::filesystem::temp_directory_path() / "cutline_invert.cube").string();
        FILE* f = std::fopen(lutPath.c_str(), "w");
        std::fprintf(f, "LUT_3D_SIZE 2\n");
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) std::fprintf(f, "%d %d %d\n", 1 - r, 1 - g, 1 - b);
        std::fclose(f);
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        Effect lut = makeEffect(fx::Lut);
        lut.path = lutPath;
        lut.params["intensity"].value = 100;
        c.effects.push_back(lut);
        golden("effect_lut_invert", s.render());
    }
    // flip: mirrored pixels; black & white: equal channels; blur / sharpen / vignette: goldens
    {
        Scene a;
        a.snap.assets[1] = asset(1, "still.png");
        a.add(a.v1, 1);
        QImage plain = a.render();
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        Effect flip = makeEffect(fx::Flip);
        flip.params["horizontal"].value = 1;
        c.effects.push_back(flip);
        QImage flipped = s.render();
        int worst = 0;
        for (int y = 40; y < plain.height() - 40; y += 37)
            for (int x = 40; x < plain.width() - 40; x += 41) {
                QRgb p = plain.pixel(x, y), q = flipped.pixel(plain.width() - 1 - x, y);
                worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q))});
            }
        report(worst <= 6, "effect: flip horizontal mirrors the frame", "max channel error " + std::to_string(worst));
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "color_full_709.mp4");
        Clip& c = s.add(s.v1, 1);
        c.effects.push_back(makeEffect(fx::BlackWhite));
        QImage img = s.render();
        int spread = 0;
        for (int y = 0; y < img.height(); y += 23)
            for (int x = 0; x < img.width(); x += 29) {
                QRgb p = img.pixel(x, y);
                spread = std::max({spread, std::abs(qRed(p) - qGreen(p)), std::abs(qGreen(p) - qBlue(p))});
            }
        report(spread <= 2, "effect: black & white has no colour left", "max channel spread " + std::to_string(spread));
        golden("effect_black_white", img);
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        Effect blur = makeEffect(fx::Blur);
        blur.params["radius"].value = 24;
        c.effects.push_back(blur);
        golden("effect_blur", s.render());
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        Effect sh = makeEffect(fx::Sharpen);
        sh.params["amount"].value = 150;
        c.effects.push_back(sh);
        Effect vig = makeEffect(fx::Vignette);
        vig.params["amount"].value = 70;
        c.effects.push_back(vig);
        golden("effect_sharpen_vignette", s.render());
    }    {
        // picture in picture: rounded corners + border over a background
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        s.snap.assets[2] = asset(2, "color_full_709.mp4");
        s.add(s.v1, 2);
        Clip& c = s.add(s.v2, 1);
        c.motion.params["scale"].value = 35;
        c.motion.params["posX"].value = 380;
        c.motion.params["posY"].value = -200;
        Effect r = makeEffect(fx::Rounded);
        r.params["radius"].value = 140;
        r.params["border"].value = 14;
        c.effects.push_back(r);
        Effect sh = makeEffect(fx::DropShadow);
        sh.params["opacity"].value = 80;
        sh.params["distance"].value = 30;
        sh.params["softness"].value = 60;
        c.effects.push_back(sh);
        golden("effect_rounded_pip", s.render());
    }    {
        // chroma key: a real-world green (not pure #00ff00) keys out; skin and grey stay
        const std::string gs = (std::filesystem::temp_directory_path() / "cutline_greenscreen.png").string();
        {
            QImage img(1280, 720, QImage::Format_RGB32);
            img.fill(QColor(52, 178, 74));
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(224, 172, 140));
            p.drawEllipse(QPointF(640, 360), 200, 200);
            p.setBrush(QColor(128, 128, 128));
            p.drawRect(80, 560, 400, 100);
            p.end();
            img.save(QString::fromStdString(gs));
        }
        Scene s;
        s.snap.assets[1] = asset(1, "color_full_709.mp4");
        AssetRef g;
        g.id = 2;
        g.path = gs;
        g.info = probeMedia(gs);
        s.snap.assets[2] = g;
        s.add(s.v1, 1);
        Clip& c = s.add(s.v2, 2);
        c.effects.push_back(makeEffect(fx::ChromaKey));
        QImage img = s.render();
        Scene bgOnly;
        bgOnly.snap.assets[1] = asset(1, "color_full_709.mp4");
        bgOnly.add(bgOnly.v1, 1);
        QImage bg = bgOnly.render();
        auto diff = [](QRgb a, QRgb b) {
            return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
        };
        const int keyed = std::max(diff(img.pixel(1100, 150), bg.pixel(1100, 150)), diff(img.pixel(150, 150), bg.pixel(150, 150)));
        const int skin = diff(img.pixel(640, 360), qRgb(224, 172, 140)), grey = diff(img.pixel(280, 610), qRgb(128, 128, 128));
        report(keyed <= 4 && skin <= 8 && grey <= 6, "effect: chroma key removes green, keeps skin and grey",
               "keyed " + std::to_string(keyed) + " skin " + std::to_string(skin) + " grey " + std::to_string(grey));
        golden("effect_chroma_key", img);
    }
    {
        // ellipse mask with feather: only the middle of the clip shows over the background
        Scene s;
        s.snap.assets[1] = asset(1, "color_full_709.mp4");
        s.snap.assets[2] = asset(2, "still.png");
        s.add(s.v1, 1);
        Clip& c = s.add(s.v2, 2);
        Effect m = makeEffect(fx::Mask);
        m.params["w"].value = 60;
        m.params["h"].value = 70;
        m.params["feather"].value = 60;
        c.effects.push_back(m);
        golden("effect_mask_ellipse", s.render());
    }    {
        // nested sequence: a nest of a clip looks exactly like the clip; effects apply to the nest
        Scene direct;
        direct.snap.assets[1] = asset(1, "still.png");
        Clip& dc = direct.add(direct.v1, 1);
        dc.motion.params["scale"].value = 60;
        QImage want = direct.render();

        auto inner = std::make_shared<RenderSnapshot>();
        inner->seq = makeDefaultSequence({1280, 720, {30, 1}, 48000});
        inner->seq.uid = 7;
        inner->assets = direct.snap.assets;
        {
            Asset tmp;
            tmp.id = 1;
            tmp.info = inner->assets[1].info;
            Clip ic = makeVideoClip(tmp, inner->seq.trackAt(TrackKind::Video, 0)->id, 0, 0, kTicksPerSecond * 4);
            ic.id = inner->seq.newId();
            ic.motion.params["scale"].value = 60;
            inner->seq.clips.push_back(ic);
        }
        Scene outer;
        outer.snap.assets = direct.snap.assets;
        outer.snap.nested[7] = inner;
        Clip n;
        n.kind = ClipKind::Nested;
        n.nestedSeq = 7;
        n.trackId = outer.v1;
        n.duration = kTicksPerSecond * 4;
        initBuiltinEffects(n, TrackKind::Video);
        n.id = outer.snap.seq.newId();
        outer.snap.seq.clips.push_back(n);
        QImage got = outer.render();
        int worst = 0;
        for (int y = 0; y < want.height(); y += 13)
            for (int x = 0; x < want.width(); x += 17) {
                QRgb a = want.pixel(x, y), b = got.pixel(x, y);
                worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
            }
        report(worst <= 3, "nested sequence renders like its contents", "max channel error " + std::to_string(worst));
        outer.snap.seq.clips.back().effects.push_back(makeEffect(fx::BlackWhite));
        outer.snap.seq.clips.back().motion.params["rotation"].value = 10;
        QImage fx = outer.render();
        int spread = 0;
        for (int y = 0; y < fx.height(); y += 23)
            for (int x = 0; x < fx.width(); x += 29) {
                QRgb p = fx.pixel(x, y);
                spread = std::max({spread, std::abs(qRed(p) - qGreen(p)), std::abs(qGreen(p) - qBlue(p))});
            }
        report(spread <= 2, "effects on a nested clip apply to everything inside", "max channel spread " + std::to_string(spread));
        golden("nested_rotated_bw", fx);
    }    {
        // colour matte: full frame in its colour; as a rounded bar with a shadow (lower third)
        Scene s;
        Clip m = makeMatteClip(s.v1, 0, kTicksPerSecond * 4, 0xFFC03030);
        m.id = s.snap.seq.newId();
        s.snap.seq.clips.push_back(m);
        QImage img = s.render();
        const QRgb px = img.pixel(640, 360);
        report(std::abs(qRed(px) - 0xC0) <= 1 && qGreen(px) <= 0x31 && qBlue(px) <= 0x31 && img.pixel(5, 5) == px,
               "color matte fills the frame", "");
        Scene bar;
        bar.snap.assets[1] = asset(1, "color_full_709.mp4");
        bar.add(bar.v1, 1);
        Clip b = makeMatteClip(bar.v2, 0, kTicksPerSecond * 4, 0xE0202838);
        b.id = bar.snap.seq.newId();
        b.motion.params["uniform"].value = 0;
        b.motion.params["scale"].value = 18;   // height
        b.motion.params["scaleW"].value = 60;  // width
        b.motion.params["posX"].value = -200;
        b.motion.params["posY"].value = 230;
        Effect r = makeEffect(fx::Rounded);
        r.params["radius"].value = 60;
        b.effects.push_back(r);
        b.effects.push_back(makeEffect(fx::DropShadow));
        bar.snap.seq.clips.push_back(b);
        golden("matte_lower_third", bar.render());
    }
    {
        // adjustment layer: its effects grade everything below, opacity blends with the original
        auto build = [&](bool adjust, double opacity, bool direct) {
            Scene s;
            s.snap.assets[1] = asset(1, "still.png");
            Clip& c = s.add(s.v1, 1);
            if (direct) c.effects.push_back(makeEffect(fx::BlackWhite));
            if (adjust) {
                Clip a = makeAdjustmentClip(s.v2, 0, kTicksPerSecond * 4);
                a.id = s.snap.seq.newId();
                a.effects.push_back(makeEffect(fx::BlackWhite));
                a.opacity.params["opacity"].value = opacity;
                s.snap.seq.clips.push_back(a);
            }
            return s.render();
        };
        const QImage graded = build(true, 100, false), direct = build(false, 100, true), plain = build(false, 100, false);
        const QImage half = build(true, 50, false);
        int worst = 0, worstHalf = 0;
        for (int y = 0; y < graded.height(); y += 11)
            for (int x = 0; x < graded.width(); x += 13) {
                const QRgb g = graded.pixel(x, y), d = direct.pixel(x, y), p = plain.pixel(x, y), h = half.pixel(x, y);
                worst = std::max({worst, std::abs(qRed(g) - qRed(d)), std::abs(qGreen(g) - qGreen(d)), std::abs(qBlue(g) - qBlue(d))});
                auto mid = [](int a, int b) { return (a + b) / 2; };
                worstHalf = std::max({worstHalf, std::abs(qRed(h) - mid(qRed(g), qRed(p))), std::abs(qGreen(h) - mid(qGreen(g), qGreen(p))),
                                      std::abs(qBlue(h) - mid(qBlue(g), qBlue(p)))});
            }
        report(worst <= 2, "adjustment layer grades the tracks below", "max channel error " + std::to_string(worst));
        report(worstHalf <= 3, "adjustment layer at 50 % blends with the original", "max channel error " + std::to_string(worstHalf));
        // with a rectangle mask it grades only inside the mask; outside stays untouched (not black)
        Scene m;
        m.snap.assets[1] = asset(1, "still.png");
        m.add(m.v1, 1);
        Clip a = makeAdjustmentClip(m.v2, 0, kTicksPerSecond * 4);
        a.id = m.snap.seq.newId();
        a.effects.push_back(makeEffect(fx::BlackWhite));
        Effect mask = makeEffect(fx::Mask);
        mask.params["shape"].value = 1;
        mask.params["feather"].value = 0;
        a.effects.push_back(mask);
        m.snap.seq.clips.push_back(a);
        const QImage masked = m.render();
        int inErr = 0, outErr = 0;
        for (int y = 0; y < masked.height(); y += 7)
            for (int x = 0; x < masked.width(); x += 9) {
                const double rx = double(x) / masked.width(), ry = double(y) / masked.height();
                const bool inside = rx > 0.27 && rx < 0.73 && ry > 0.27 && ry < 0.73;
                const bool outside = rx < 0.23 || rx > 0.77 || ry < 0.23 || ry > 0.77;
                const QRgb v = masked.pixel(x, y), ref = inside ? graded.pixel(x, y) : plain.pixel(x, y);
                const int e = std::max({std::abs(qRed(v) - qRed(ref)), std::abs(qGreen(v) - qGreen(ref)), std::abs(qBlue(v) - qBlue(ref))});
                if (inside) inErr = std::max(inErr, e);
                if (outside) outErr = std::max(outErr, e);
            }
        report(inErr <= 2 && outErr <= 2, "masked adjustment layer grades only inside its mask",
               "inside error " + std::to_string(inErr) + ", outside error " + std::to_string(outErr));
    }
    {
        // typewriter: half revealed = the left part of the same layout (nothing moves)
        auto build = [&](double progress) {
            Scene s;
            TextStyle st;
            st.size = 120;
            Clip& c = s.addText(s.v1, "TYPEWRITER", st);
            Effect tw = makeEffect(fx::Typewriter);
            tw.params["progress"].value = progress;
            c.effects.push_back(tw);
            return s.render();
        };
        auto inkX = [](const QImage& im, int& x0, int& x1) {
            x0 = im.width();
            x1 = -1;
            for (int y = 0; y < im.height(); y += 2)
                for (int x = 0; x < im.width(); ++x)
                    if (qRed(im.pixel(x, y)) > 128) x0 = std::min(x0, x), x1 = std::max(x1, x);
        };
        int f0, f1, h0, h1, z0, z1;
        inkX(build(100), f0, f1);
        inkX(build(50), h0, h1);
        inkX(build(0), z0, z1);
        const double frac = double(h1 - h0) / std::max(1, f1 - f0);
        report(std::abs(h0 - f0) <= 2 && frac > 0.35 && frac < 0.65 && z1 < 0, "typewriter reveals the title from the left in place",
               "full " + std::to_string(f0) + ".." + std::to_string(f1) + ", half " + std::to_string(h0) + ".." + std::to_string(h1) +
                   ", none " + std::to_string(z1));
    }
    {
        // mosaic: neighbouring pixels mostly equal (blocks), unlike the plain picture
        auto build = [&](bool mosaic) {
            Scene s;
            s.snap.assets[1] = asset(1, "still.png");
            Clip& c = s.add(s.v1, 1);
            if (mosaic) {
                Effect m = makeEffect(fx::Mosaic);
                m.params["size"].value = 32;
                c.effects.push_back(m);
            }
            return s.render();
        };
        auto edges = [](const QImage& im) {
            int n = 0;
            for (int y = 0; y < im.height(); y += 3)
                for (int x = 0; x + 1 < im.width(); ++x) {
                    const QRgb a = im.pixel(x, y), b = im.pixel(x + 1, y);
                    n += std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b)) > 12;
                }
            return n;
        };
        const int plainEdges = edges(build(false)), mosaicEdges = edges(build(true));
        report(mosaicEdges > 0 && mosaicEdges * 4 < plainEdges, "mosaic turns the picture into blocks",
               std::to_string(mosaicEdges) + " vs " + std::to_string(plainEdges) + " colour steps");
    }    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        Clip& c = s.add(s.v1, 1);
        Effect g = makeEffect(fx::Glow);
        g.params["threshold"].value = 50;
        g.params["intensity"].value = 150;
        c.effects.push_back(g);
        golden("effect_glow", s.render());
    }
    {
        Scene s;
        TextStyle st = titleStyle();
        st.gradient = true;
        st.fill = 0xFFFFE070;
        st.fill2 = 0xFFFF5020;
        st.boxRadius = 40;
        st.tracking = 12;
        st.stroke = false;
        s.addText(s.v1, "GG EZ", st);
        golden("title_gradient_rounded", s.render());
    }
    // --- transitions ---------------------------------------------------------------------------
    for (TransitionType tt : {TransitionType::CrossDissolve, TransitionType::DipToBlack, TransitionType::DipToWhite,
                              TransitionType::WipeLeft, TransitionType::WipeRight, TransitionType::WipeUp,
                              TransitionType::WipeDown, TransitionType::Slide, TransitionType::Push, TransitionType::CrossZoom,
                              TransitionType::IrisRound, TransitionType::BlurDissolve, TransitionType::Whip}) {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        s.snap.assets[2] = asset(2, "color_full_709.mp4");
        Clip& a = s.add(s.v1, 1, 0, kTicksPerSecond * 2);
        Id aid = a.id;
        Clip& b = s.add(s.v1, 2, kTicksPerSecond * 2, kTicksPerSecond * 2);
        Id bid = b.id;
        // a 1 s transition centred on the cut at 2 s: 0% at 1.5 s, 50% at 2 s, 100% at 2.5 s
        edit::addTransition(s.snap.seq, s.v1, aid, bid, tt, kTicksPerSecond, TransitionAlign::Center);
        std::string base = std::string("transition_") + transitionName(tt);
        for (char& ch : base)
            if (ch == ' ') ch = '_';
        const Tick fd = s.snap.seq.frameDur();
        golden(base + "_000", s.render(kTicksPerSecond * 3 / 2));
        golden(base + "_050", s.render(kTicksPerSecond * 2));
        golden(base + "_100", s.render(kTicksPerSecond * 5 / 2 - fd));
    }
    // --- titles and captions -------------------------------------------------------------------
    {
        Scene s;
        TextStyle st = titleStyle();
        s.addText(s.v1, "Cutline Title\nsecond line", st);
        golden("title_stroke_shadow_box", s.render(), 16, 0.01);
    }
    {
        Scene s;
        s.snap.assets[1] = asset(1, "still.png");
        s.add(s.v1, 1);
        edit::addCaption(s.snap.seq, 0, kTicksPerSecond, "Burned-in caption");
        golden("caption_burn_in", s.render(kTicksPerSecond / 2), 16, 0.01);
        QImage after = s.render(kTicksPerSecond + kTicksPerSecond / 2);
        // caption gone after its end: bottom area matches the plain still
        Scene plain;
        plain.snap.assets[1] = asset(1, "still.png");
        plain.add(plain.v1, 1);
        QImage ref = plain.render(kTicksPerSecond + kTicksPerSecond / 2);
        report(after == ref, "caption ends on the right frame", "");
    }
    // --- NV12 conversion round trip ---------------------------------------------------------
    {
        auto canvas = comp.makeCanvas(256, 128, false);
        comp.clear(*canvas, 0xC0 / 255.f, 0x30 / 255.f, 0x20 / 255.f, 1);
        auto nv12 = g_gpu->acquire(256, 128, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET);
        comp.toNV12(*canvas, *nv12, ColorMatrix::BT709);
        std::vector<uint8_t> d;
        int pitch = 0;
        comp.readback(*nv12, d, pitch);
        double Y = (d[size_t(64) * pitch + 64] - 16) / 219.0;
        double Cb = (d[size_t(128 + 32) * pitch + 64] - 128) / 224.0;
        double Cr = (d[size_t(128 + 32) * pitch + 65] - 128) / 224.0;
        double R = Y + 1.5748 * Cr, G = Y - 0.1873 * Cb - 0.4681 * Cr, B = Y + 1.8556 * Cb;
        int err8 = int(std::lround(std::max({std::abs(R * 255 - 0xC0), std::abs(G * 255 - 0x30), std::abs(B * 255 - 0x20)})));
        report(err8 <= 3, "rgb->nv12 (BT.709 limited)", "max error " + std::to_string(err8));
    }
    std::printf("\nrendertest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
