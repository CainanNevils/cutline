// uishots: renders panels offscreen with a project loaded and a clip selected, for visual review.
//   uishots <project.cutproj> <outdir>
#include "core/edits.h"
#include "ui/audio_mixer_panel.h"
#include "ui/captions_panel.h"
#include "ui/dialogs.h"
#include "ui/effect_controls.h"
#include "ui/effects_panel.h"
#include "ui/history_panel.h"
#include "ui/scopes_panel.h"
#include "ui/session.h"
#include "ui/theme.h"
#include "ui/timeline_widget.h"

#include <QApplication>
#include <QDir>
#include <QThread>
#include <QElapsedTimer>
#include <QWidget>

#include <cstdio>

using namespace cut;

static void shot(QWidget* w, const QString& file, QSize size) {
    w->resize(size);
    w->show();
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    w->grab().save(file);
    std::printf("wrote %s\n", file.toLocal8Bit().constData());
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    qputenv("CUTLINE_TEST", "1");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("CutlineShots");
    ui::applyTheme(app);
    if (argc < 3) return 1;
    QString out = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(out);
    ui::Session s;
    QString err;
    if (!s.openProject(QString::fromLocal8Bit(argv[1]), err)) {
        std::printf("%s\n", err.toLocal8Bit().constData());
        return 1;
    }
    // add some content so every panel has something to show
    Id v1clip = 0, title = 0;
    for (const auto& c : s.seq().clips) {
        const Track* t = s.seq().track(c.trackId);
        if (t->kind == TrackKind::Video && c.kind == ClipKind::Media && !v1clip) v1clip = c.id;
        if (c.kind == ClipKind::Text) title = c.id;
    }
    s.edit("demo", [&](Project& p) {
        Clip* c = p.sequence.clip(v1clip);
        c->motion.params["posX"].addKey(0, -200, Interp::Ease);
        c->motion.params["posX"].addKey(kTicksPerSecond * 20, 200);
        Effect lut = makeEffect(fx::Lut);
        c->effects.push_back(makeEffect(fx::Crop));
        c->effects.push_back(lut);
        edit::addCaption(p.sequence, 0, kTicksPerSecond * 2, "Hello captions");
        edit::addCaption(p.sequence, kTicksPerSecond * 3, kTicksPerSecond * 5, "Second line");
        edit::addMarker(p.sequence, kTicksPerSecond * 10, "Highlight");
        p.sequence.inPoint = kTicksPerSecond * 5;
        p.sequence.outPoint = kTicksPerSecond * 60;
    });
    QElapsedTimer wait;
    wait.start();
    while ((s.jobs().running() || s.jobs().pending()) && wait.elapsed() < 60000) {
        app.processEvents();
        QThread::msleep(20);
    }
    s.setSelection({v1clip});
    s.setPlayhead(kTicksPerSecond * 7);
    ui::EffectControls fx(&s);
    for (auto* lane : fx.findChildren<ui::KeyLane*>())
        if (lane->animated()) lane->setGraph(true);
    shot(&fx, out + "/effect_controls_video.png", {560, 900});
    if (title) {
        s.setSelection({title});
        ui::EffectControls fx2(&s);
        shot(&fx2, out + "/effect_controls_title.png", {560, 1100});
    }
    {
        Id aclip = 0;
        for (const auto& c : s.seq().clips)
            if (s.seq().track(c.trackId)->kind == TrackKind::Audio && !aclip) aclip = c.id;
        s.edit("audio fx", [&](Project& p) {
            Clip* c = p.sequence.clip(aclip);
            c->effects.push_back(makeEffect(fx::HighPass));
            c->effects.push_back(makeEffect(fx::Compressor));
        });
        s.setSelection({aclip}, false);
        ui::EffectControls fxa(&s);
        shot(&fxa, out + "/effect_controls_audio.png", {560, 760});
    }
    ui::AudioMixerPanel mixer(&s);
    shot(&mixer, out + "/audio_mixer.png", {600, 420});
    ui::CaptionsPanel caps(&s);
    shot(&caps, out + "/captions.png", {600, 300});
    ui::EffectsPanel effects(&s);
    shot(&effects, out + "/effects.png", {320, 420});
    ui::ExportDialog exp(&s);
    shot(&exp, out + "/export_dialog.png", exp.sizeHint().expandedTo({600, 700}));
    ui::PreferencesDialog prefs(&s);
    shot(&prefs, out + "/preferences.png", prefs.sizeHint().expandedTo({560, 300}));
    s.setSelection({v1clip});
    ui::SpeedDialog speed(&s);
    shot(&speed, out + "/speed_dialog.png", speed.sizeHint().expandedTo({360, 200}));
    // scopes from a synthetic frame: 75 % colour bars over a luma ramp, plus a skin-ish patch
    {
        QImage bars(480, 270, QImage::Format_RGBA8888);
        const QColor cols[7] = {QColor(191, 191, 191), QColor(191, 191, 0), QColor(0, 191, 191), QColor(0, 191, 0),
                                QColor(191, 0, 191), QColor(191, 0, 0), QColor(0, 0, 191)};
        for (int y = 0; y < 270; ++y)
            for (int x = 0; x < 480; ++x) {
                QColor c = y < 180 ? cols[x * 7 / 480] : QColor::fromRgbF(x / 479.0, x / 479.0, x / 479.0);
                if (y >= 230 && x >= 380) c = QColor(224, 172, 140);
                bars.setPixelColor(x, y, c);
            }
        ui::ScopesPanel scopes(&s);
        const char* names[] = {"waveform", "parade", "vectorscope", "histogram"};
        for (int m = 0; m < 4; ++m) {
            scopes.setMode(ui::ScopesPanel::Mode(m));
            scopes.setFrame(bars);
            shot(&scopes, out + "/scopes_" + names[m] + ".png", {560, 380});
        }
    }
    s.edit("Move Clip", [&](Project& p) { p.sequence.clips.front().start += kTicksPerSecond; });
    s.edit("Trim Clip", [&](Project& p) { p.sequence.clips.front().start -= kTicksPerSecond; });
    s.undo();
    ui::HistoryPanel hist(&s);
    shot(&hist, out + "/history.png", {320, 300});
    s.edit("markers", [&](Project& p) {
        edit::addMarker(p.sequence, kTicksPerSecond * 30, "Second act");
        edit::addMarker(p.sequence, kTicksPerSecond * 2);
        p.sequence.markers.back().color = 0xFFE5534B;
    });
    ui::MarkersPanel marks(&s);
    shot(&marks, out + "/markers.png", {420, 240});
    s.setSelection({v1clip});
    ui::TimelineWidget tl(&s);
    tl.resize(1900, 640);
    tl.show();
    tl.zoomToFit();
    shot(&tl, out + "/timeline_selected.png", {1900, 640});
    return 0;  // widgets are destroyed before the session (declared first)
}
