// uitest: drives the real widgets with synthetic mouse/keyboard input and checks the model.
//   uitest            timeline interactions (offscreen) + main window shortcuts (real window)
//   uitest --timeline / --window
#include "core/edits.h"
#include "core/project_io.h"
#include "engine/frame_renderer.h"
#include "media/frame_reader.h"
#include "media/probe.h"
#include "render/compositor.h"
#include "ui/dialogs.h"
#include "ui/effect_controls.h"
#include "ui/effects_panel.h"
#include "ui/export_queue.h"
#include "ui/history_panel.h"
#include "ui/main_window.h"
#include "ui/project_panel.h"
#include "ui/scopes_panel.h"
#include "ui/shortcuts_dialog.h"
#include "ui/session.h"
#include "ui/theme.h"
#include "ui/timeline_widget.h"
#include "ui/video_widget.h"

#include <QApplication>
#include <QDir>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QUrl>
#include <QDockWidget>
#include <QLineEdit>
#include <QGroupBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QListWidget>
#include <QTreeView>
#include <QTreeWidget>
#include <QMimeData>
#include <QPainter>
#include <QTimer>
#include <QMenu>
#include <QStackedWidget>
#include <QListView>
#include <QMouseEvent>
#include <QTabBar>
#include <QTextBrowser>
#include <QThread>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <functional>
#include <string>

using namespace cut;
using ui::Session;

namespace {
constexpr Tick S = kTicksPerSecond;
int g_pass = 0, g_fail = 0;
std::string g_media = CUTLINE_TESTMEDIA;

void check(bool ok, const std::string& name, const std::string& detail = {}) {
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

void pump(int ms = 0) {
    QElapsedTimer t;
    t.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (ms) QThread::msleep(2);
    } while (t.elapsed() < ms);
}

void mouse(QWidget* w, QEvent::Type type, QPoint p, Qt::MouseButton b, Qt::MouseButtons bs,
           Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent e(type, QPointF(p), w->mapToGlobal(QPointF(p)), b, bs, mods);
    QApplication::sendEvent(w, &e);
}
void click(QWidget* w, QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouse(w, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
    pump();
}
void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
    const int steps = 8;
    for (int i = 1; i <= steps; ++i) {
        QPoint p = from + (to - from) * i / steps;
        mouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, mods);
    }
    mouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
    pump();
}
void key(QWidget* w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
    // shortcuts are delivered through ShortcutOverride + KeyPress like real typing
    QKeyEvent over(QEvent::ShortcutOverride, k, mods, text);
    QApplication::sendEvent(w, &over);
    QKeyEvent press(QEvent::KeyPress, k, mods, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent rel(QEvent::KeyRelease, k, mods, text);
    QApplication::sendEvent(w, &rel);
    pump(20);
}

std::vector<const Clip*> on(const Session& s, TrackKind k, int idx) {
    const Track* t = s.seq().trackAt(k, idx);
    return t ? s.seq().clipsOnTrack(t->id) : std::vector<const Clip*>{};
}
Id track(const Session& s, TrackKind k, int idx) { return s.seq().trackAt(k, idx)->id; }
std::string secs(Tick t) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f s", ticksToSeconds(t));
    return b;
}

// project: main clip [0,20) linked to game+mic audio, ultrawide clip [20,30)
std::string makeProject() {
    Project p;
    p.sequence = makeDefaultSequence({2560, 1440, {60, 1}, 48000});
    auto add = [&](const char* f) {
        Asset a;
        a.id = p.newAssetId();
        a.path = g_media + "/" + f;
        a.name = f;
        a.info = probeMedia(a.path);
        p.assets.push_back(a);
        return a;
    };
    Asset m = add("main_1440p60_h264.mkv");
    Asset u = add("uw_3440x1440p60_hevc.mkv");
    edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, m, 0, 0, 20 * S, 0, 0));
    edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, u, 20 * S, 0, 10 * S, 0, 0));
    edit::normalize(p.sequence);
    std::string path = (QDir::tempPath() + "/cutline_uitest.cutproj").toStdString();
    std::string err;
    saveProjectFile(p, path, err);
    return path;
}

void timelineTests() {
    std::printf("--- timeline (offscreen widget) ---\n");
    Session s;
    QString err;
    if (!s.openProject(QString::fromStdString(makeProject()), err)) {
        check(false, "open project", err.toStdString());
        return;
    }
    s.snapping = false;
    ui::TimelineWidget tl(&s);
    tl.resize(1800, 600);
    tl.show();
    pump(50);
    tl.zoomToFit();
    pump(250);
    // zoom bar: dragging the thumb's right end to the left zooms in (start stays put)
    {
        QScrollBar* hb = nullptr;
        for (auto* sb : tl.findChildren<QScrollBar*>())
            if (sb->orientation() == Qt::Horizontal) hb = sb;
        const double pps0 = tl.pixelsPerSecond();
        const double total = double(hb->maximum()) + hb->pageStep();
        const int xr = int(1 + (hb->width() - 2) * (hb->value() + hb->pageStep()) / total) - 2;
        drag(hb, QPoint(xr, hb->height() / 2), QPoint(xr / 2, hb->height() / 2));
        const double ratio = tl.pixelsPerSecond() / pps0;
        check(ratio > 1.6 && ratio < 2.6 && hb->value() == 0, "zoom bar: dragging the end zooms in about 2x",
              std::to_string(ratio));
        tl.zoomToFit();
        pump(50);
    }
    const Id v1 = track(s, TrackKind::Video, 0), v2 = track(s, TrackKind::Video, 1), a1 = track(s, TrackKind::Audio, 0);
    const double pps = tl.pixelsPerSecond();
    check(pps > 20, "zoom to fit", "pps " + std::to_string(pps));

    // 1. click selects the clip and its linked audio
    click(&tl, tl.pointFor(v1, 5 * S));
    check(s.selection().size() == 3, "click selects clip + linked game/mic audio", std::to_string(s.selection().size()) + " selected");
    // Alt-click selects only that clip
    click(&tl, tl.pointFor(v1, 5 * S), Qt::AltModifier);
    check(s.selection().size() == 1, "Alt-click ignores links", std::to_string(s.selection().size()) + " selected");

    // 2. razor tool splits video + linked audio
    s.setTool(Session::Tool::Razor);
    click(&tl, tl.pointFor(v1, 10 * S));
    auto v = on(s, TrackKind::Video, 0);
    check(v.size() == 3 && v[0]->end() == s.snapToFrame(v[0]->end()), "razor splits V1 at the click",
          std::to_string(v.size()) + " clips, cut at " + (v.size() > 1 ? secs(v[1]->start) : std::string("-")));
    check(on(s, TrackKind::Audio, 0).size() == 3 && on(s, TrackKind::Audio, 1).size() == 2, "razor splits linked audio too (game + mic)");
    const Tick cut = v[1]->start;
    check(std::llabs(cut - 10 * S) <= Tick(2.0 / pps * S), "cut lands under the cursor (+-2 px)", secs(cut));
    s.setTool(Session::Tool::Select);

    // 3. drag the middle clip 2 s right (overwrite mode) and onto V2
    Id mid = v[1]->id;
    QPoint from = tl.pointFor(v1, cut + 3 * S);
    QPoint to = tl.pointFor(v2, cut + 5 * S);
    drag(&tl, from, to);
    const Clip* mc = s.seq().clip(mid);
    check(mc && mc->trackId == v2, "drag moves the clip to V2", mc ? std::to_string(s.seq().trackIndexInKind(mc->trackId)) : "gone");
    check(mc && std::llabs(mc->start - (cut + 2 * S)) <= Tick(2.0 / pps * S), "drag moves it 2 s later", mc ? secs(mc->start) : "");
    // linked audio moved in time but stayed on its audio tracks
    bool audioFollowed = true;
    for (Id l : s.seq().linkedClips(mid))
        if (const Clip* c = s.seq().clip(l); c && c->id != mid)
            audioFollowed &= c->start == mc->start && s.seq().track(c->trackId)->kind == TrackKind::Audio;
    check(audioFollowed, "linked audio follows in time on its own tracks");
    check(s.doc().undo.undoLabel() == "Move", "the whole drag is one undo step", s.doc().undo.undoLabel());
    s.undo();
    pump();
    check(s.seq().clip(mid)->trackId == v1 && s.seq().clip(mid)->start == cut, "undo puts it back");

    // 4. trim the tail of the first clip 2 s left (plain trim leaves a gap)
    v = on(s, TrackKind::Video, 0);
    Id first = v[0]->id;
    Tick firstEnd = v[0]->end();
    drag(&tl, tl.pointFor(v1, firstEnd) + QPoint(-2, 0), tl.pointFor(v1, firstEnd - 2 * S) + QPoint(-2, 0));
    check(std::llabs(s.seq().clip(first)->end() - (firstEnd - 2 * S)) <= Tick(3.0 / pps * S), "trim tail by dragging the edge",
          secs(s.seq().clip(first)->end()));
    check(s.seq().clip(mid)->start == cut, "plain trim leaves the next clip in place");
    s.undo();
    pump();

    // 5. ripple tool: same drag closes the gap
    s.setTool(Session::Tool::Ripple);
    drag(&tl, tl.pointFor(v1, firstEnd) + QPoint(-2, 0), tl.pointFor(v1, firstEnd - 2 * S) + QPoint(-2, 0));
    Tick shift = cut - s.seq().clip(mid)->start;
    check(std::llabs(shift - 2 * S) <= Tick(3.0 / pps * S), "ripple trim shifts later clips left", secs(shift));
    s.setTool(Session::Tool::Select);
    s.undo();
    pump();

    // 6. rubber band over V1 + A1 from empty space
    click(&tl, tl.pointFor(v1, 40 * S));  // empty: deselect
    check(s.selection().empty(), "click on empty space clears the selection");
    QPoint rb0 = tl.pointFor(v1, 9 * S) + QPoint(0, -20), rb1 = tl.pointFor(a1, 11 * S);
    rb0.setX(tl.pointFor(v1, 45 * S).x());  // start in empty area to the right
    drag(&tl, rb0, rb1);
    check(s.selection().size() == 5, "rubber band selects across tracks (+ linked mic)", std::to_string(s.selection().size()) + " selected");
    click(&tl, tl.pointFor(v1, 45 * S));

    // 7. audio fade-in handle: drag the top-left square right by 1 s
    auto a = on(s, TrackKind::Audio, 0);
    const Clip* ac = a[0];
    QPoint fh = tl.pointFor(a1, ac->start) + QPoint(5, 0);
    fh.setY(tl.pointFor(a1, ac->start).y() - (s.seq().track(a1)->height - 7) + 5);  // top of the clip
    drag(&tl, fh, fh + QPoint(int(pps), 0));
    check(std::llabs(s.seq().clip(ac->id)->fadeIn - S) <= Tick(4.0 / pps * S), "drag fade handle sets fade-in",
          secs(s.seq().clip(ac->id)->fadeIn));

    // 8. delete + ripple delete via the session commands the shortcuts call
    click(&tl, tl.pointFor(v1, cut + S));
    s.deleteSelection(true);
    pump();
    v = on(s, TrackKind::Video, 0);
    check(v.size() == 2 && v[1]->start == cut, "ripple delete closes the gap", v.size() == 2 ? secs(v[1]->start) : "");

    // 9. drop an asset from the Project panel (drag enter / move / drop with Ctrl = insert)
    auto* md = new QMimeData;
    md->setData("application/x-cutline-assets", QByteArray::number(qulonglong(s.project().assets[1].id)));
    QPoint dp = tl.pointFor(v1, 0);
    const size_t before = s.seq().clips.size();
    QDragEnterEvent de(dp, Qt::CopyAction, md, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&tl, &de);
    QDragMoveEvent dm(dp, Qt::CopyAction, md, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&tl, &dm);
    QDropEvent dr(QPointF(dp), Qt::CopyAction, md, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&tl, &dr);
    pump();
    v = on(s, TrackKind::Video, 0);
    check(s.seq().clips.size() == before + 2 && v.front()->assetId == s.project().assets[1].id, "drop from Project panel inserts at 0",
          std::to_string(s.seq().clips.size() - before) + " clips added");
    if (s.seq().clips.size() != before + 2)
        for (const auto& c : s.seq().clips)
            std::printf("    clip %llu track %s start %s dur %s asset %llu\n", (unsigned long long)c.id,
                        s.seq().track(c.trackId)->name.c_str(), secs(c.start).c_str(), secs(c.duration).c_str(),
                        (unsigned long long)c.assetId);
    check(v.size() >= 2 && v[1]->start == v[0]->end(), "insert drop pushed the rest right");
    delete md;
    s.shutdown();
}

void windowTests() {
    std::printf("--- main window (real window + engine) ---\n");
    Session s;
    QString err;
    if (!s.startEngine(err)) {
        check(false, "engine start", err.toStdString());
        return;
    }
    ui::MainWindow w(&s);
    w.resize(1600, 950);
    w.show();
    w.activateWindow();
    pump(300);
    if (!s.openProject(QString::fromStdString(makeProject()), err)) {
        check(false, "open project", err.toStdString());
        return;
    }
    pump(1500);
    auto* tl = w.findChild<ui::TimelineWidget*>();
    tl->setFocus();
    pump(50);
    const Id v1 = track(s, TrackKind::Video, 0);
    // CUTLINE_UITEST_SHOTS=<dir>: the whole window as composited on screen (monitors included) at a few points
    auto shot = [&](const char* name) {
        const QString dir = qEnvironmentVariable("CUTLINE_UITEST_SHOTS");
        if (dir.isEmpty()) return;
        pump(800);
        const QRect g = w.frameGeometry();
        w.screen()->grabWindow(0, g.x(), g.y(), g.width(), g.height()).save(dir + "/" + name + ".png");
    };
    if (!qEnvironmentVariable("CUTLINE_UITEST_SHOTS").isEmpty()) {
        s.setSelection({on(s, TrackKind::Video, 0).front()->id});
        s.setPlayhead(4 * S);
        shot("main");
        s.setSelection({});
    }

    // Ctrl+K at the playhead
    s.setPlayhead(6 * S);
    key(tl, Qt::Key_K, Qt::ControlModifier);
    check(on(s, TrackKind::Video, 0).size() == 3, "Ctrl+K adds an edit at the playhead");
    // Ctrl+Z undoes it
    key(tl, Qt::Key_Z, Qt::ControlModifier);
    check(on(s, TrackKind::Video, 0).size() == 2, "Ctrl+Z undoes");
    key(tl, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(on(s, TrackKind::Video, 0).size() == 3, "Ctrl+Shift+Z redoes");
    // I / O mark the sequence
    s.setPlayhead(2 * S);
    key(tl, Qt::Key_I, Qt::NoModifier, "i");
    s.setPlayhead(4 * S);
    key(tl, Qt::Key_O, Qt::NoModifier, "o");
    check(s.seq().inPoint == 2 * S && s.seq().outPoint == 4 * S + s.seq().frameDur(), "I / O set sequence in/out");
    // M marker
    key(tl, Qt::Key_M, Qt::NoModifier, "m");
    check(s.seq().markers.size() == 1 && s.seq().markers[0].t == 4 * S, "M adds a marker at the playhead");
    // arrows / Up/Down
    key(tl, Qt::Key_Right);
    check(s.playhead() == 4 * S + s.seq().frameDur(), "Right steps one frame");
    key(tl, Qt::Key_Down);
    check(s.playhead() == 6 * S, "Down jumps to the next edit point", secs(s.playhead()));
    key(tl, Qt::Key_Home);
    check(s.playhead() == 0, "Home goes to the start");
    // tools by letter
    key(tl, Qt::Key_C, Qt::NoModifier, "c");
    check(s.tool() == Session::Tool::Razor, "C selects the razor");
    key(tl, Qt::Key_V, Qt::NoModifier, "v");
    check(s.tool() == Session::Tool::Select, "V selects the selection tool");
    // select a clip, Shift+E disables, Ctrl+L unlinks
    click(tl, tl->pointFor(v1, S));
    Id sel = s.primaryClip();
    key(tl, Qt::Key_E, Qt::ShiftModifier, "E");
    check(sel && !s.seq().clip(sel)->enabled, "Shift+E disables the clip");
    key(tl, Qt::Key_L, Qt::ControlModifier);
    check(sel && s.seq().clip(sel)->linkId == 0, "Ctrl+L unlinks");
    // Ctrl+T title at the playhead, on a free track
    s.setPlayhead(S);
    key(tl, Qt::Key_T, Qt::ControlModifier);
    bool title = false;
    for (const auto& c : s.seq().clips) title |= c.kind == ClipKind::Text && c.start == S;
    check(title, "Ctrl+T adds a title at the playhead");
    // Ctrl+D default transition at the nearest cut on the targeted video track
    s.setPlayhead(6 * S);
    key(tl, Qt::Key_D, Qt::ControlModifier);
    check(!s.seq().transitions.empty(), "Ctrl+D adds a cross dissolve at the nearest cut");
    // Space plays, Space again stops; playhead moves
    s.setPlayhead(0);
    key(tl, Qt::Key_Space, Qt::NoModifier, " ");
    pump(1200);
    bool wasPlaying = s.playing();
    Tick moved = s.playhead();
    key(tl, Qt::Key_Space, Qt::NoModifier, " ");
    pump(200);
    check(wasPlaying && !s.playing() && moved > S / 2, "Space plays and stops", "played to " + secs(moved));
    ViewerStats st = s.engine()->stats(s.programViewer());
    check(st.framesShown > 30 && st.dropped == 0, "Program monitor rendered frames while playing",
          std::to_string(st.framesShown) + " shown, " + std::to_string(st.dropped) + " dropped");
    // L shuttles faster, K stops
    key(tl, Qt::Key_L, Qt::NoModifier, "l");
    key(tl, Qt::Key_L, Qt::NoModifier, "l");
    pump(100);
    check(s.engine()->playSpeed() == 2.0, "L L = 2x shuttle", std::to_string(s.engine()->playSpeed()));
    key(tl, Qt::Key_K, Qt::NoModifier, "k");
    pump(100);
    check(!s.playing(), "K stops");
    // Lift ( ; ) leaves a gap, Extract ( ' ) closes it
    s.edit("in/out", [](Project& p) {
        p.sequence.inPoint = 8 * S;
        p.sequence.outPoint = 9 * S;
    });
    const Tick durBefore = s.seq().duration();
    key(tl, Qt::Key_Semicolon, Qt::NoModifier, ";");
    bool gap = true;
    for (const Clip* c : on(s, TrackKind::Video, 0))
        if (c->start < 9 * S && c->end() > 8 * S) gap = false;
    check(gap && s.seq().duration() == durBefore, "; lifts the In-Out range (gap, same length)");
    key(tl, Qt::Key_Z, Qt::ControlModifier);
    key(tl, Qt::Key_Apostrophe, Qt::NoModifier, "'");
    check(s.seq().duration() == durBefore - S, "' extracts the In-Out range (sequence 1 s shorter)", secs(durBefore - s.seq().duration()));
    // Q / W ripple trim the previous / next edit to the playhead (all unlocked tracks stay in sync)
    {
        auto expectRange = [&](Tick t, bool prev) {
            Tick a = kInvalidTick, b = kInvalidTick;
            for (const auto& c : s.seq().clips) {
                if (!(c.start < t && c.end() > t)) continue;
                a = a == kInvalidTick ? c.start : std::max(a, c.start);
                b = b == kInvalidTick ? c.end() : std::min(b, c.end());
            }
            return prev ? std::pair{a, t} : std::pair{t, b};
        };
        const Tick tq = 3 * S + S / 2;
        s.setPlayhead(tq);
        auto [qa, qb] = expectRange(tq, true);
        const Tick d0 = s.seq().duration();
        key(tl, Qt::Key_Q, Qt::NoModifier, "q");
        check(qa != kInvalidTick && s.seq().duration() == d0 - (qb - qa) && s.playhead() == qa,
              "Q ripple trims the previous edit to the playhead", secs(d0 - s.seq().duration()) + " removed");
        key(tl, Qt::Key_Z, Qt::ControlModifier);
        const Tick tw = 2 * S + S / 2;
        s.setPlayhead(tw);
        auto [wa, wb] = expectRange(tw, false);
        key(tl, Qt::Key_W, Qt::NoModifier, "w");
        check(wb != kInvalidTick && s.seq().duration() == d0 - (wb - wa) && s.playhead() == tw,
              "W ripple trims the next edit to the playhead", secs(d0 - s.seq().duration()) + " removed");
        key(tl, Qt::Key_Z, Qt::ControlModifier);
        check(s.seq().duration() == d0, "undo restores the ripple trim");
    }
    // History panel: click a step to go back to it, click the last one to return
    if (auto* hp = w.findChild<ui::HistoryPanel*>()) {
        const size_t pos = s.doc().undo.position();
        const std::string snapshot = s.seq().clips.size() ? std::to_string(s.seq().duration()) : "";
        hp->jumpTo(pos - 2);
        pump(50);
        const bool back = s.doc().undo.position() == pos - 2 && hp->list()->currentRow() == int(pos - 2);
        hp->jumpTo(pos);
        pump(50);
        check(back && s.doc().undo.position() == pos && std::to_string(s.seq().duration()) == snapshot,
              "History panel jumps back two steps and forward again");
    }
    // Markers panel lists the markers and renames them in place (undoable)
    if (auto* mp = w.findChild<ui::MarkersPanel*>()) {
        pump(50);
        const bool listed = mp->tree()->topLevelItemCount() == int(s.seq().markers.size()) && !s.seq().markers.empty();
        if (listed) mp->tree()->topLevelItem(0)->setText(1, "Renamed");
        pump(50);
        check(listed && s.seq().markers[0].name == "Renamed" && s.doc().undo.undoLabel() == "Rename Marker",
              "Markers panel lists and renames markers");
    }
    // Scopes: raising the panel turns on the engine read-back and fills the scope
    if (auto* sp = w.findChild<ui::ScopesPanel*>()) {
        auto* dock = w.findChild<QDockWidget*>("scopes");
        const bool hiddenFirst = !sp->active();
        sp->setFrame(QImage());
        dock->show();
        dock->raise();
        s.setPlayhead(2 * S);
        QElapsedTimer st2;
        st2.start();
        while (sp->scopeImage().isNull() && st2.elapsed() < 5000) pump(20);
        const bool filled = !sp->scopeImage().isNull();
        sp->setMode(ui::ScopesPanel::Mode::Vectorscope);
        check(hiddenFirst && filled && !sp->scopeImage().isNull(), "Scopes panel shows the Program frame (waveform, vectorscope)",
              std::to_string(st2.elapsed()) + " ms" + (hiddenFirst ? "" : " (was active)") + (filled ? "" : " (no frame)"));
        w.findChild<QDockWidget*>("source")->raise();
    }
    // Changing the audio device restarts the engine; monitors must come back
    {
        QString rerr;
        const bool rok = s.restartEngine(rerr);
        pump(400);
        s.setPlayhead(2 * S);
        pump(400);
        const ViewerStats rs = s.engine()->stats(s.programViewer());
        check(rok && rs.framesShown > 0 && s.engine()->audioStatus().ok, "engine restart (new audio device) keeps monitors and audio working",
              rerr.toStdString() + " frames " + std::to_string(rs.framesShown));
    }
    // Close All Gaps: a lifted range leaves a hole on every track; the command removes it
    {
        const Tick d0 = s.seq().duration();
        s.edit("in/out", [](Project& p) {
            p.sequence.inPoint = 5 * S;
            p.sequence.outPoint = 6 * S;
        });
        s.liftExtract(false);
        s.closeAllGaps();
        check(s.seq().duration() == d0 - S && s.doc().undo.undoLabel() == "Close Gaps", "Close All Gaps removes the lifted hole",
              secs(d0 - s.seq().duration()) + " shorter");
    }
    // Paste Attributes: motion + effects from one clip onto another
    {
        auto vclips = on(s, TrackKind::Video, 0);
        if (vclips.size() >= 2) {
            const Id a = vclips[0]->id, b = vclips[1]->id;
            s.edit("fx", [&](Project& p) {
                Clip* c = p.sequence.clip(a);
                c->motion.params["scale"].value = 150;
                c->effects.push_back(makeEffect(fx::Color));
            });
            s.setSelection({a}, false);
            s.copySelection();
            s.setSelection({b}, false);
            s.pasteAttributes(true, true, true, true);
            const Clip* cb = s.seq().clip(b);
            check(cb && cb->motion.params.at("scale").value == 150 && cb->effects.size() == 1, "Paste Attributes copies motion and effects");
        } else check(false, "Paste Attributes copies motion and effects", "need two V1 clips");
    }
    // Effect presets: save a configured effect, apply it from the Effects panel; search filters
    if (auto* ep = w.findChild<ui::EffectsPanel*>()) {
        auto vclips = on(s, TrackKind::Video, 0);
        Effect look = makeEffect(fx::Color);
        look.params["saturation"].value = 42;
        s.saveEffectPreset("UITest Look", look);
        pump(20);
        QTreeWidgetItem* item = nullptr;
        QTreeWidgetItem* crop = nullptr;
        QTreeWidgetItem* dissolve = nullptr;
        std::function<void(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) {
            if (it->text(0) == "UITest Look") item = it;
            if (it->text(0) == "Crop") crop = it;
            if (it->text(0) == "Cross Dissolve") dissolve = it;
            for (int i = 0; i < it->childCount(); ++i) find(it->child(i));
        };
        for (int i = 0; i < ep->tree()->topLevelItemCount(); ++i) find(ep->tree()->topLevelItem(i));
        const Id target = vclips.back()->id;
        s.setSelection({target}, false);
        if (item) emit ep->tree()->itemDoubleClicked(item, 0);
        bool applied = false;
        for (const auto& e : s.seq().clip(target)->effects)
            applied |= e.type == fx::Color && e.params.count("saturation") && e.params.at("saturation").value == 42;
        check(item && applied, "effect preset saved and applied from the Effects panel");
        // audio effects go onto audio clips only
        QTreeWidgetItem* gate = nullptr;
        std::function<void(QTreeWidgetItem*)> findGate = [&](QTreeWidgetItem* it) {
            if (it->text(0) == "Noise Gate") gate = it;
            for (int i = 0; i < it->childCount(); ++i) findGate(it->child(i));
        };
        for (int i = 0; i < ep->tree()->topLevelItemCount(); ++i) findGate(ep->tree()->topLevelItem(i));
        const Id aclip = on(s, TrackKind::Audio, 0).front()->id;
        s.setSelection({aclip, target}, false);
        if (gate) emit ep->tree()->itemDoubleClicked(gate, 0);
        auto hasGate = [&](Id id) {
            for (const auto& e : s.seq().clip(id)->effects)
                if (e.type == fx::NoiseGate) return true;
            return false;
        };
        check(gate && hasGate(aclip) && !hasGate(target), "audio effect applies to the audio clip, not the video one");
        ep->setFilter("dissolve");
        check(dissolve && crop && !dissolve->isHidden() && crop->isHidden(), "Effects search filters the list");
        ep->setFilter("");
        s.removeEffectPreset("UITest Look");
    }
    // Markers -> YouTube chapters: first line at 0:00, then each marker
    {
        Sequence sq = s.seq();
        sq.markers.clear();
        edit::addMarker(sq, 75 * S, "Boss fight");
        edit::addMarker(sq, 30 * S);
        const QString ch = ui::youtubeChapters(sq);
        check(ch == "0:00 Intro\n0:30 Chapter 1\n1:15 Boss fight", "markers copy as YouTube chapters", ch.toStdString());
    }
    // X marks the clip under the playhead; D selects it
    {
        s.setPlayhead(2 * S + S / 2);
        key(tl, Qt::Key_X, Qt::NoModifier, "x");
        const Clip* under = s.clipAtPlayhead();
        check(under && s.seq().inPoint == under->start && s.seq().outPoint == under->end(), "X marks the clip under the playhead");
        key(tl, Qt::Key_D, Qt::NoModifier, "d");
        check(under && s.primaryClip() == under->id, "D selects the clip under the playhead");
    }
    // Sequences: a new tab is empty and independent; switching keeps each one intact
    if (auto* tabs = w.findChild<QTabBar*>("sequenceTabs")) {
        const size_t clipsBefore = s.seq().clips.size();
        const std::string firstName = s.seq().name;
        s.newSequence(s.seq().settings, "Shorts");
        pump(50);
        const bool fresh = s.seq().clips.empty() && tabs->count() == 2 && tabs->currentIndex() == 1 && tabs->tabText(1) == "Shorts";
        s.edit("title", [&](Project& p) {
            edit::overwriteClips(p.sequence, {makeTextClip(p.sequence.trackAt(TrackKind::Video, 0)->id, 0, 2 * S, "Hi")});
        });
        emit tabs->tabBarClicked(0);
        pump(50);
        const bool back = s.seq().name == firstName && s.seq().clips.size() == clipsBefore;
        s.undo();  // undoes the switch: back in "Shorts" with its title
        pump(20);
        const bool undone = s.seq().name == "Shorts" && s.seq().clips.size() == 1;
        s.deleteSequence(1);
        pump(20);
        check(fresh && back && undone && tabs->count() == 1 && s.seq().clips.size() == clipsBefore,
              "sequence tabs: new, switch, undo switch, delete");
    }
    // Nest: the selected clip (and its linked audio) becomes one nested clip; double-click opens it
    {
        const Id vclip = on(s, TrackKind::Video, 0).front()->id;
        const Tick vstart = s.seq().clip(vclip)->start;
        s.setSelection({vclip});
        s.nestSelection();
        pump(50);
        const Clip* nest = nullptr;
        for (const auto& c : s.seq().clips)
            if (c.kind == ClipKind::Nested && s.seq().track(c.trackId)->kind == TrackKind::Video) nest = &c;
        auto* tabs = w.findChild<QTabBar*>("sequenceTabs");
        const bool made = nest && nest->start == vstart && tabs && tabs->count() == 2;
        // the program monitor and Export Frame render through the nest
        s.setPlayhead(vstart + S / 2);
        pump(300);
        QString png = QDir::tempPath() + "/cutline_uitest_nest.png";
        QFile::remove(png);
        QString ferr;
        const bool exported = s.exportFrame(png, ferr) && QImage(png).width() == 2560;
        const Id nid = nest ? nest->id : 0;
        click(tl, tl->pointFor(nest ? nest->trackId : 0, vstart + S / 2));
        mouse(tl, QEvent::MouseButtonDblClick, tl->pointFor(nest ? nest->trackId : 0, vstart + S / 2), Qt::LeftButton, Qt::LeftButton);
        pump(100);
        const bool opened = tabs && tabs->currentIndex() == 1 && !s.seq().clips.empty();
        check(made && exported && opened && nid, "Nest: clip becomes a nested sequence, renders, opens on double-click",
              std::string(made ? "" : "not made ") + (exported ? "" : "export failed ") + (opened ? "" : "not opened"));
        // inside the nest, the outer sequence cannot be nested (it would contain itself)
        const Id outerUid = s.sequenceUids().front();
        const bool cycleRefused = !s.canNest(outerUid) && s.placeSequence(outerUid, 0, 0, 0, false).empty();
        s.switchSequence(0);
        pump(50);
        // from the outer sequence, the nest can be placed again (drag from the Project panel)
        const Id nestUid = s.sequenceUids().back();
        const auto placed = s.placeSequence(nestUid, s.seq().duration(), 0, 0, false);
        check(cycleRefused && !placed.empty(), "sequences nest by drag, never into themselves",
              std::string(cycleRefused ? "" : "cycle allowed ") + std::to_string(placed.size()) + " placed, " + std::to_string(s.sequenceUids().size()) + " seqs, active " + std::to_string(s.activeSequence()));
        s.undo();
    }
    // Graphics > New Adjustment Layer lands above the clips at the playhead
    {
        s.setPlayhead(2 * S);
        QAction* adj = nullptr;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->text() == "New Adjustment Layer") adj = a;
        if (adj) adj->trigger();
        pump(100);
        const Clip* layer = nullptr;
        for (const auto& c : s.seq().clips)
            if (c.kind == ClipKind::Adjustment) layer = &c;
        bool above = layer != nullptr;
        if (layer)
            for (const auto& c : s.seq().clips)
                if (c.kind == ClipKind::Media && s.seq().track(c.trackId)->kind == TrackKind::Video && c.contains(2 * S))
                    above &= s.seq().trackIndexInKind(layer->trackId) > s.seq().trackIndexInKind(c.trackId);
        check(adj && layer && above && layer->start == 2 * S, "New Adjustment Layer goes above the clips at the playhead");
        if (layer) s.undo();
    }
    // Remove Silence: the 1.5 s pause goes (minus the kept padding), the 0.2 s one stays
    {
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({QString::fromStdString(g_media + "/pauses.wav")}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer it;
        it.start();
        while (!done && it.elapsed() < 20000) pump(20);
        const Asset* pa = imported.empty() ? nullptr : s.project().asset(imported.front());
        bool ok = false;
        std::string detail = "import failed";
        if (pa) {
            s.newSequence(s.seq().settings, "Silence");
            Id clip = 0;
            s.edit("place", [&](Project& p) {
                auto clips = edit::clipsForAsset(p.sequence, *pa, 0, 0, pa->info.duration, 0, 0);
                clip = edit::overwriteClips(p.sequence, clips).front();
            });
            s.setSelection({clip});
            while ((!s.waveform(pa->id, 0) || !s.waveform(pa->id, 0)->complete.load()) && it.elapsed() < 30000) pump(20);
            const Tick before = s.seq().duration();
            const int n = s.removeSilence(s.silenceSourceClip(), -40, S / 2, S * 15 / 100);
            const double cut = ticksToSeconds(before - s.seq().duration());
            ok = n == 1 && std::abs(cut - 1.2) < 0.05;
            detail = std::to_string(n) + " pauses, " + std::to_string(cut) + " s cut";
            s.deleteSequence(s.activeSequence());
        }
        check(ok, "Remove Silence cuts the long pause, keeps padding and the short pause", detail);
    }
    // Collect Files: project + used media into a fresh folder, and the copy opens with media online
    {
        const QString dir = QDir::tempPath() + "/cutline_collect_test";
        QDir(dir).removeRecursively();
        Session::CollectProgress prog;
        QString cerr;
        Project mini;  // (the session's media is gigabytes; a mini project keeps the test quick)
        mini.sequence = makeDefaultSequence();
        Asset pa;
        pa.id = mini.newAssetId();
        pa.path = g_media + "/pauses.wav";
        pa.name = "pauses.wav";
        pa.info = probeMedia(pa.path);
        mini.assets.push_back(pa);
        Asset unused = pa;
        unused.id = mini.newAssetId();
        unused.path = g_media + "/music.mp3";
        unused.info = probeMedia(unused.path);
        mini.assets.push_back(unused);
        edit::overwriteClips(mini.sequence, edit::clipsForAsset(mini.sequence, pa, 0, 0, pa.info.duration, 0, 0));
        const bool ok = Session::collectFiles(mini, "Collected", dir, true, prog, cerr);
        Project back;
        std::string lerr;
        bool online = ok && loadProjectFile(prog.projectPath.toStdString(), back, lerr);
        if (online) {
            resolveMediaPaths(back, prog.projectPath.toStdString());
            for (const auto& a : back.assets) online &= !a.offline && QString::fromStdString(a.path).startsWith(QDir::toNativeSeparators(dir));
            online &= back.assets.size() == 1;  // only the used file

        }
        check(ok && online && prog.done.load() == prog.total.load() && prog.total.load() > 0, "Collect Files copies the project and its media",
              cerr.toStdString() + " " + std::to_string(prog.total.load()) + " bytes, ok " + std::to_string(ok) + " assets " + std::to_string(back.assets.size()) + " " + prog.projectPath.toStdString());
        QDir(dir).removeRecursively();
    }
    // Markers at beats (120 BPM click track) and Enhance Voice on the same clip
    {
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({QString::fromStdString(g_media + "/beats.wav")}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer bt;
        bt.start();
        while (!done && bt.elapsed() < 20000) pump(20);
        const Asset* ba = imported.empty() ? nullptr : s.project().asset(imported.front());
        int beats = 0;
        size_t fx = 0;
        if (ba) {
            s.newSequence(s.seq().settings, "Beats");
            Id clip = 0;
            s.edit("place", [&](Project& p) {
                clip = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, *ba, 0, 0, ba->info.duration, 0, 0)).front();
            });
            s.setSelection({clip});
            while ((!s.waveform(ba->id, 0) || !s.waveform(ba->id, 0)->complete.load()) && bt.elapsed() < 30000) pump(20);
            beats = s.addBeatMarkers();
            s.enhanceVoice();
            fx = s.seq().clip(clip)->effects.size();
            s.deleteSequence(s.activeSequence());
            s.switchSequence(0);
        }
        check(beats >= 7 && beats <= 8, "Add Markers at Beats finds the 120 BPM kicks", std::to_string(beats) + " markers");
        check(fx == 4, "Enhance Voice adds the four-effect chain", std::to_string(fx) + " effects");
    }
    // Keyframe value graph: Shift-drag a key up changes its value, not its time
    {
        const Id vc = on(s, TrackKind::Video, 0).front()->id;
        s.edit("keys", [&](Project& p) {
            Param& op = p.sequence.clip(vc)->opacity.params["opacity"];
            op.keys.clear();
            op.addKey(0, 20);
            op.addKey(S, 80);
        });
        s.setSelection({vc}, false);
        w.findChild<QDockWidget*>("fxcontrols")->raise();
        pump(100);
        ui::KeyLane* lane = nullptr;
        for (auto* l : w.findChildren<ui::KeyLane*>())
            if (l->animated() && l->isVisible()) lane = l;
        bool ok = false;
        std::string detail = "no lane";
        if (lane) {
            lane->setGraph(true);
            pump(50);
            const QPointF k = lane->keyPoint(0, 20);
            mouse(lane, QEvent::MouseButtonPress, k.toPoint(), Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 6; ++i) mouse(lane, QEvent::MouseMove, (k - QPointF(0, 4.0 * i)).toPoint(), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
            mouse(lane, QEvent::MouseButtonRelease, (k - QPointF(0, 24)).toPoint(), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
            pump(50);
            const Param& op = s.seq().clip(vc)->opacity.params.at("opacity");
            ok = op.keys.size() == 2 && op.keys[0].t == 0 && op.keys[0].v > 25;
            detail = op.keys.empty() ? "no keys" : "key 0 now " + std::to_string(op.keys[0].v) + " at " + secs(op.keys[0].t);
        }
        check(ok, "value graph: Shift-drag raises a keyframe's value", detail);
        s.undo();
        s.undo();
        w.findChild<QDockWidget*>("source")->raise();
    }
    // Rate Stretch (R): dragging a clip's end longer slows it down, same media
    {
        auto vs = on(s, TrackKind::Video, 0);
        const Clip* last = vs.back();
        const Id id = last->id;
        const Tick end0 = last->end(), span0 = last->sourceSpan();
        key(tl, Qt::Key_R, Qt::NoModifier, "r");
        const QPoint from = tl->pointFor(last->trackId, end0) - QPoint(2, 0);
        drag(tl, from, from + QPoint(int(tl->pixelsPerSecond() * 2), 0));  // 2 s longer
        const Clip* c = s.seq().clip(id);
        const bool ok = s.tool() == Session::Tool::RateStretch && c && c->end() > end0 + S && c->speed < 0.95 &&
                        std::llabs(c->sourceSpan() - span0) <= 2 * s.seq().frameDur();
        check(ok, "Rate Stretch tool: longer clip, slower speed, same footage",
              c ? std::to_string(c->speed) + " speed, " + secs(c->end() - end0) + " longer" : "gone");
        s.undo();
        key(tl, Qt::Key_V, Qt::NoModifier, "v");
    }
    // Cut at Scene Changes on a three-scene clip -> three pieces
    {
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({QString::fromStdString(g_media + "/scenes.mp4")}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer st3;
        st3.start();
        while (!done && st3.elapsed() < 20000) pump(20);
        size_t pieces = 0;
        if (const Asset* sa = imported.empty() ? nullptr : s.project().asset(imported.front())) {
            s.newSequence(s.seq().settings, "Scenes");
            Id clip = 0;
            s.edit("place", [&](Project& p) {
                clip = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, *sa, 0, 0, sa->info.duration, 0, 0)).front();
            });
            s.setSelection({clip});
            s.detectScenes(true);
            while (s.doc().undo.undoLabel() != "Cut at Scene Changes" && st3.elapsed() < 60000) pump(50);
            pieces = on(s, TrackKind::Video, 0).size();
            s.deleteSequence(s.activeSequence());
            s.switchSequence(0);
        }
        check(pieces == 3, "Cut at Scene Changes splits a three-scene clip", std::to_string(pieces) + " pieces");
    }
    // Match frame: F opens the clip under the playhead in the Source monitor at the same frame
    s.setPlayhead(3 * S);
    key(tl, Qt::Key_F, Qt::NoModifier, "f");
    check(s.sourceAsset() != 0 && std::llabs(s.sourcePosition() - 3 * S) <= s.seq().frameDur(), "F = match frame",
          secs(s.sourcePosition()));
    // Export frame writes a PNG of the sequence size
    QString png = QDir::tempPath() + "/cutline_uitest_frame.png";
    QFile::remove(png);
    QString ferr;
    bool fok = s.exportFrame(png, ferr);
    QImage fimg(png);
    check(fok && fimg.width() == 2560 && fimg.height() == 1440, "Export Frame writes a full-size PNG",
          QString("%1x%2 %3").arg(fimg.width()).arg(fimg.height()).arg(ferr).toStdString());
    // Remove Unused Media: only media no sequence uses (and not what Source shows)
    {
        const auto unused = s.unusedAssets();
        const auto used = s.usedAssets();
        bool disjoint = true;
        for (Id u : unused)
            for (Id x : used) disjoint = disjoint && u != x;
        const size_t before = s.project().assets.size();
        s.removeAssets(unused);
        const bool gone = s.project().assets.size() == before - unused.size() && s.unusedAssets().empty();
        bool clipsIntact = true;
        for (const auto& c : s.seq().clips)
            if (c.kind == ClipKind::Media) clipsIntact = clipsIntact && s.project().asset(c.assetId);
        check(disjoint && gone && clipsIntact && s.usedAssets() == used, "Remove Unused Media keeps everything in use",
              std::to_string(unused.size()) + " removed of " + std::to_string(before));
        if (!unused.empty()) s.undo();
    }
    // Help > User Guide (F1): the README, rendered from the embedded resource
    {
        QAction* g = nullptr;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->text() == "User Guide") g = a;
        if (g) g->trigger();
        pump(100);
        QTextBrowser* tb = nullptr;
        for (QWidget* tw : QApplication::topLevelWidgets())
            if (auto* x = tw->findChild<QTextBrowser*>("guideText")) tb = x;
        const QString txt = tb ? tb->toPlainText() : QString();
        check(g && g->shortcut() == QKeySequence(Qt::Key_F1) && txt.contains("Instant Replay") && txt.contains("Getting started"),
              "Help > User Guide (F1) shows the README", std::to_string(txt.size()) + " characters");
        if (tb) tb->window()->close();
        pump(50);
    }
    // Export Thumbnail: 1280x720 JPEG under 2 MB
    {
        const QString jpg = QDir::tempPath() + "/cutline_uitest_thumb.jpg";
        QFile::remove(jpg);
        QString terr;
        const bool tok = s.exportThumbnail(jpg, terr);
        const QImage t(jpg);
        check(tok && t.width() == 1280 && t.height() == 720 && QFileInfo(jpg).size() < 2'000'000, "Export Thumbnail writes a 1280x720 JPEG",
              terr.toStdString());
        QFile::remove(jpg);
    }
    // Normalize the game audio clip to -14 LUFS (tone is around -30 LUFS -> about +16 dB)
    for (const Clip* c : on(s, TrackKind::Audio, 0))
        if (c->contains(3 * S)) s.setSelection({c->id}, false);
    s.normalizeSelectedAudio(-14);
    QElapsedTimer nt;
    nt.start();
    while (s.doc().undo.undoLabel() != "Normalize Audio" && nt.elapsed() < 30000) pump(20);
    double g = s.selection().empty() ? 0 : s.seq().clip(s.selection()[0])->gainDb;
    check(g > 8 && g < 24, "Normalize to -14 LUFS sets clip gain", std::to_string(g) + " dB");
    // Drop a file from Explorer onto the timeline end
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(QString::fromStdString(g_media + "/music.mp3"))});
    const size_t assetsBefore = s.project().assets.size();
    const Tick endT = s.seq().duration();
    QPoint dp = tl->pointFor(track(s, TrackKind::Audio, 2), endT);
    QDragEnterEvent de(dp, Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &de);
    QDropEvent dr(QPointF(dp), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &dr);
    nt.restart();
    while (s.project().assets.size() == assetsBefore && nt.elapsed() < 20000) pump(20);
    pump(100);
    bool placed = false;
    for (const Clip* c : on(s, TrackKind::Audio, 2)) placed |= c->assetId == s.project().assets.back().id;
    check(s.project().assets.size() == assetsBefore + 1 && placed, "file dropped from Explorer is imported and placed on A3");
    // Synchronize (by Audio): a second recording of part of the same moment (quieter, inverted,
    // its own noise) moves so its sound lines up with the first, to the millisecond
    {
        auto writeWav = [](const QString& path, const std::vector<float>& mono) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return;
            auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
            const uint32_t bytes = uint32_t(mono.size() * 2);
            f.write("RIFF", 4);
            u32(36 + bytes);
            f.write("WAVEfmt ", 8);
            u32(16);
            u16(1);
            u16(1);
            u32(48000);
            u32(96000);
            u16(2);
            u16(16);
            f.write("data", 4);
            u32(bytes);
            std::vector<int16_t> pcm(mono.size());
            for (size_t i = 0; i < mono.size(); ++i) pcm[i] = int16_t(std::clamp(mono[i], -1.f, 1.f) * 32767.f);
            f.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * 2));
        };
        std::mt19937 rng(5);
        std::normal_distribution<float> gn(0.f, 1.f);
        std::uniform_real_distribution<double> u(0, 1);
        std::vector<float> scene(size_t(40 * 48000));
        for (auto& v : scene) v = 0.002f * gn(rng);
        for (double t = 0.2; t < 39;) {
            const double len = 0.08 + 0.5 * u(rng), amp = 0.1 + 0.5 * u(rng);
            float lp = 0;
            for (size_t i = size_t(t * 48000); i < size_t((t + len) * 48000); ++i) {
                lp += 0.3f * (gn(rng) - lp);
                scene[i] += float(amp) * lp;
            }
            t += len + 0.05 + 0.6 * u(rng);
        }
        const double off = 9.1234;
        std::vector<float> mic(size_t(20 * 48000));
        for (size_t i = 0; i < mic.size(); ++i) mic[i] = -0.4f * scene[size_t(off * 48000) + i] + 0.004f * gn(rng);
        const QString dir = QDir::tempPath() + "/cutline_uitest_sync";
        QDir().mkpath(dir);
        writeWav(dir + "/game.wav", scene);
        writeWav(dir + "/mic.wav", mic);
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({dir + "/game.wav", dir + "/mic.wav"}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer syt;
        syt.start();
        while (!done && syt.elapsed() < 20000) pump(20);
        double syncErr = 1e9;
        int synced = 0;
        if (imported.size() == 2) {
            const Asset ga = *s.project().asset(imported[0]), ma = *s.project().asset(imported[1]);
            s.newSequence(s.seq().settings, "Sync");
            Id gc = 0, mc = 0;
            s.edit("place", [&](Project& p) {
                gc = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, ga, 0, 0, ga.info.duration, 0, 0)).front();
                mc = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, ma, 30 * S, 0, ma.info.duration, 0, 1)).front();
            });
            s.setSelection({gc, mc});
            while ((!s.waveform(ga.id, 0) || !s.waveform(ga.id, 0)->complete.load() || !s.waveform(ma.id, 0) ||
                    !s.waveform(ma.id, 0)->complete.load()) && syt.elapsed() < 30000)
                pump(20);
            synced = s.synchronizeSelection();
            const Clip* m = s.seq().clip(mc);
            const Clip* gg = s.seq().clip(gc);
            if (m && gg) syncErr = ticksToSeconds((m->start - m->sourceIn) - (gg->start - gg->sourceIn)) - off;
            s.deleteSequence(s.activeSequence());
            s.switchSequence(0);
        }
        check(synced == 1 && std::abs(syncErr) < 0.001, "Synchronize (by Audio) lines a second recording up to the millisecond",
              "error " + std::to_string(syncErr * 1000) + " ms");
        QDir(dir).removeRecursively();
    }
    // Project panel: double-clicking a clip whose thumbnail has loaded opens it in the Source monitor
    // (the thumbnail flag once shared a role with sequence items and made clips look like sequences)
    {
        ui::ProjectPanel* pp = w.findChild<ui::ProjectPanel*>();
        QTreeView* tv = pp ? pp->findChild<QTreeView*>() : nullptr;
        Id mainId = 0;
        for (const Asset& a : s.project().assets)
            if (a.name == "main_1440p60_h264.mkv") mainId = a.id;
        QElapsedTimer tt;
        tt.start();
        while (mainId && !s.thumbs(mainId) && tt.elapsed() < 20000) pump(50);
        pump(600);  // the panel's status refresh picks the thumbnail up
        QModelIndex hit;
        if (tv)
            for (int r = 0; r < tv->model()->rowCount(); ++r) {
                const QModelIndex i = tv->model()->index(r, 0);
                if (i.data().toString() == "main_1440p60_h264.mkv") hit = i;
            }
        const int tabsBefore = s.activeSequence();
        for (const Asset& a : s.project().assets)
            if (a.name == "uw_3440x1440p60_hevc.mkv") s.openInSource(a.id);  // something else first
        if (hit.isValid()) {
            tv->scrollTo(hit);
            const QPoint c = tv->visualRect(hit).center();
            mouse(tv->viewport(), QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton, {});
            mouse(tv->viewport(), QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton, {});
            mouse(tv->viewport(), QEvent::MouseButtonDblClick, c, Qt::LeftButton, Qt::LeftButton, {});
            mouse(tv->viewport(), QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton, {});
            pump(100);
        }
        check(hit.isValid() && s.sourceAsset() == mainId && s.activeSequence() == tabsBefore,
              "double-clicking a clip with its thumbnail loaded opens it in Source",
              "source " + std::to_string(s.sourceAsset()) + " want " + std::to_string(mainId));
    }
    // Project panel icon view: hovering scrubs the thumbnail; double-click opens Source at that moment
    {
        ui::ProjectPanel* pp = w.findChild<ui::ProjectPanel*>();
        QListView* lv = pp ? pp->findChild<QListView*>() : nullptr;
        QStackedWidget* stk = lv ? qobject_cast<QStackedWidget*>(lv->parentWidget()) : nullptr;
        if (stk) stk->setCurrentWidget(lv);
        pump(100);
        QModelIndex hit;
        Id mainId = 0;
        Tick dur = 0;
        for (const Asset& a : s.project().assets)
            if (a.name == "main_1440p60_h264.mkv") mainId = a.id, dur = a.info.duration;
        if (lv)
            for (int r = 0; r < lv->model()->rowCount(); ++r)
                if (lv->model()->index(r, 0).data().toString() == "main_1440p60_h264.mkv") hit = lv->model()->index(r, 0);
        Tick pos = -1;
        if (hit.isValid()) {
            lv->scrollTo(hit);
            const QRect r = lv->visualRect(hit);
            const QPoint p(r.left() + r.width() * 8 / 10, r.top() + r.height() / 3);
            mouse(lv->viewport(), QEvent::MouseMove, p - QPoint(20, 0), Qt::NoButton, Qt::NoButton, {});
            mouse(lv->viewport(), QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, {});
            mouse(lv->viewport(), QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, {});
            mouse(lv->viewport(), QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, {});
            mouse(lv->viewport(), QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton, {});
            mouse(lv->viewport(), QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, {});
            pump(100);
            pos = s.sourcePosition();
        }
        check(hit.isValid() && s.sourceAsset() == mainId && pos > dur / 2 && pos < dur,
              "icon view hover-scrub: double-click opens Source where the thumbnail was scrubbed to",
              secs(pos) + " of " + secs(dur));
        if (stk) stk->setCurrentIndex(0);
    }
    // Graphics > New Blur Region: only the rectangle in the middle changes in the exported frame
    {
        s.setPlayhead(5 * S);
        const QString before = QDir::tempPath() + "/cutline_uitest_blur0.png", after = QDir::tempPath() + "/cutline_uitest_blur1.png";
        QString e1, e2;
        s.exportFrame(before, e1);
        QAction* blurAct = nullptr;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->text() == "New Blur Region") blurAct = a;
        if (blurAct) blurAct->trigger();
        pump(50);
        const Clip* bc = s.selection().empty() ? nullptr : s.seq().clip(s.selection().front());
        s.exportFrame(after, e2);
        const QImage i0(before), i1(after);
        int inside = 0, outside = 0;
        if (!i0.isNull() && i0.size() == i1.size())
            for (int y = 0; y < i0.height(); y += 9)
                for (int x = 0; x < i0.width(); x += 11) {
                    const double rx = double(x) / i0.width(), ry = double(y) / i0.height();
                    const QRgb a = i0.pixel(x, y), b = i1.pixel(x, y);
                    const int d = std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
                    if (std::abs(rx - 0.5) < 0.12 && std::abs(ry - 0.5) < 0.06) inside = std::max(inside, d);
                    if (std::abs(rx - 0.5) > 0.2 || std::abs(ry - 0.5) > 0.12) outside = std::max(outside, d);
                }
        check(blurAct && bc && bc->kind == ClipKind::Adjustment && bc->effects.size() == 2 && inside > 30 && outside <= 3,
              "New Blur Region blurs only its rectangle", "inside diff " + std::to_string(inside) + ", outside diff " + std::to_string(outside));
        // its mask has handles in the Program monitor: drag the middle to move it, a corner to size it
        ui::VideoWidget* pv = nullptr;
        for (auto* vw : w.findChildren<ui::VideoWidget*>())
            if (vw->isProgram()) pv = vw;
        double cx = 0, mw = 0;
        if (pv && bc) {
            const Id bid = bc->id;
            const double W = s.seq().settings.width, H = s.seq().settings.height;
            const QPoint from = pv->toWidget({W / 2, H / 2}).toPoint(), to = pv->toWidget({W * 0.6, H / 2}).toPoint();
            drag(pv, from, to);
            const QPoint corner = pv->toWidget({W * 0.75, H * 0.58}).toPoint(), corner2 = pv->toWidget({W * 0.85, H * 0.58}).toPoint();
            drag(pv, corner, corner2);
            if (const Clip* c = s.seq().clip(bid))
                for (const auto& e : c->effects)
                    if (e.type == fx::Mask) cx = e.p("cx", 0), mw = e.p("w", 0);
        }
        shot("blur_region");
        check(std::abs(cx - 60) < 1.5 && std::abs(mw - 50) < 2, "the Blur Region's mask moves and resizes in the Program monitor",
              "cx " + std::to_string(cx) + " w " + std::to_string(mw));
        s.undo();
        s.undo();
        s.undo();
        QFile::remove(before);
        QFile::remove(after);
    }
    // Export dialog: an audio-only preset turns the video settings off and the file into .mp3
    {
        ui::ExportDialog d(&s, &w);
        QComboBox* preset = nullptr;
        for (auto* cb : d.findChildren<QComboBox*>())
            if (cb->findText("Audio only - MP3 320 kbps") >= 0) preset = cb;
        QLineEdit* path = d.findChildren<QLineEdit*>().value(0);
        QGroupBox* video = nullptr;
        for (auto* grp : d.findChildren<QGroupBox*>())
            if (grp->title() == "Video") video = grp;
        bool ok = preset && path && video;
        QString mp3, back;
        bool videoOff = false, videoOn = false;
        if (ok) {
            preset->setCurrentIndex(preset->findText("Audio only - MP3 320 kbps"));
            mp3 = path->text();
            videoOff = !video->isEnabled();
            preset->setCurrentIndex(0);
            back = path->text();
            videoOn = video->isEnabled();
        }
        check(ok && mp3.endsWith(".mp3") && videoOff && back.endsWith(".mp4") && videoOn,
              "Export dialog: audio-only presets switch the video off and the file type over", (mp3 + " / " + back).toStdString());
    }
    // Auto Reframe: in a vertical version, the frame follows a bright square across a 16:9 shot
    {
        s.newSequence(s.seq().settings, "Action");
        const int W = s.seq().settings.width, H = s.seq().settings.height;
        auto sqX = [](Tick t) { return -1000.0 + 2000.0 * std::clamp(ticksToSeconds(t) / 6.0, 0.0, 1.0); };
        s.edit("action", [&](Project& p) {
            Sequence& q = p.sequence;
            Clip m = makeMatteClip(q.trackAt(TrackKind::Video, 0)->id, 0, 6 * S, 0xFFFFFFFF);
            m.motion.params["scale"].value = 12;
            m.motion.params["posX"].addKey(0, sqX(0));
            m.motion.params["posX"].addKey(6 * S, sqX(6 * S));
            m.motion.params["posY"].value = std::sin(1.0) * 100;
            edit::overwriteClips(q, {m});
        });
        s.createVerticalVersion();
        Id nest = 0;
        for (const auto& c : s.seq().clips)
            if (c.kind == ClipKind::Nested && s.seq().track(c.trackId)->kind == TrackKind::Video) nest = c.id;
        const int keys = s.autoReframe(nest);
        // where the square shows in the vertical frame: its source x scaled by Fill, plus Position X
        const double fill = double(s.seq().settings.height) / H;
        double worst = 0;
        if (const Clip* c = s.seq().clip(nest))
            for (Tick t = 2 * S; t <= 4 * S; t += S / 2) {  // (away from the ends, where the smoothing is one-sided)
                const double onScreen = sqX(t) * fill + c->motion.p("posX", t);
                worst = std::max(worst, std::abs(onScreen));
            }
        check(keys >= 6 && worst < s.seq().settings.width * 0.3, "Auto Reframe keeps the moving subject in the vertical frame",
              std::to_string(keys) + " keys, subject at most " + std::to_string(worst) + " px from the centre of a " +
                  std::to_string(s.seq().settings.width) + " px frame");
        s.deleteSequence(s.activeSequence());
        s.deleteSequence(s.activeSequence());
        s.switchSequence(0);
        (void)W;
    }
    // Track Mask Forward: a Blur Region over a moving name follows it
    {
        s.newSequence(s.seq().settings, "Tracking");
        const int W = s.seq().settings.width;
        auto posAt = [](Tick t) { return -600.0 + 1200.0 * std::clamp(ticksToSeconds(t), 0.0, 4.0) / 4.0; };  // the title's X offset
        Id regionId = 0;
        s.edit("setup", [&](Project& p) {
            Sequence& q = p.sequence;
            Clip t = makeTextClip(q.trackAt(TrackKind::Video, 0)->id, 0, 5 * S, "PLAYER_ONE");
            t.textStyle.size = 220;
            t.textStyle.weight = 900;
            t.motion.params["posX"].addKey(0, posAt(0));
            t.motion.params["posX"].addKey(4 * S, posAt(4 * S));
            edit::overwriteClips(q, {t});
            Clip r = makeAdjustmentClip(q.trackAt(TrackKind::Video, 1)->id, S / 2, 4 * S);
            Effect blur = makeEffect(fx::Blur);
            Effect mask = makeEffect(fx::Mask);
            mask.params["shape"].value = 1;
            mask.params["cx"].value = (W / 2.0 + posAt(S / 2)) / W * 100;
            mask.params["w"].value = 30;
            mask.params["h"].value = 16;
            r.effects = {blur, mask};
            regionId = edit::overwriteClips(q, {r}).front();
        });
        s.setPlayhead(S / 2);
        const int keys = s.trackMaskForward(regionId);
        double worst = 0;
        Tick last = 0;
        if (const Clip* r = s.seq().clip(regionId))
            for (const auto& e : r->effects)
                if (e.type == fx::Mask && e.params.count("cx"))
                    for (const auto& k : e.params.at("cx").keys) {
                        const double want = (W / 2.0 + posAt(r->start + k.t)) / W * 100;
                        worst = std::max(worst, std::abs(k.v - want));
                        last = std::max(last, k.t);
                    }
        check(keys > 30 && worst < 1.0 && last > 3 * S, "Track Mask Forward follows a moving title",
              std::to_string(keys) + " keys over " + secs(last) + ", worst " + std::to_string(worst) + " % of the width");
        s.deleteSequence(s.activeSequence());
        s.switchSequence(0);
    }
    // Clip > Stabilize: a shaky (nested) shot comes out steady
    {
        QImage img(1280, 720, QImage::Format_RGB32);
        std::mt19937 rng(9);
        std::uniform_int_distribution<int> u(0, 255);
        img.fill(QColor(70, 80, 90));
        {
            QPainter pt(&img);
            for (int k = 0; k < 400; ++k) {
                pt.fillRect(QRect(u(rng) * 5, u(rng) * 3, 10 + u(rng) / 4, 10 + u(rng) / 4), QColor(u(rng), u(rng), u(rng)));
            }
        }
        const QString still = QDir::tempPath() + "/cutline_uitest_texture.png";
        img.save(still);
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({still}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer it;
        it.start();
        while (!done && it.elapsed() < 20000) pump(20);
        double rawJitter = 0, leftJitter = 0;
        int keys = 0;
        if (!imported.empty()) {
            const Asset tex = *s.project().asset(imported.front());
            // the shaky shot: the texture jumping around every other frame
            s.newSequence(s.seq().settings, "Shaky");
            const Id shakyUid = s.seq().uid;
            const Tick F2 = 2 * s.seq().frameDur();
            std::map<Tick, std::pair<double, double>> shake;
            std::normal_distribution<double> jolt(0, 18);
            s.edit("shake", [&](Project& p) {
                Clip c = edit::clipsForAsset(p.sequence, tex, 0, 0, 4 * S, 0, 0).front();
                c.fit = FitMode::Fill;
                for (Tick t = 0; t < 4 * S; t += F2) {
                    shake[t] = {jolt(rng), jolt(rng)};
                    c.motion.params["posX"].addKey(t, shake[t].first);
                    c.motion.params["posY"].addKey(t, shake[t].second);
                }
                edit::overwriteClips(p.sequence, {c});
            });
            s.newSequence(s.seq().settings, "Steady");
            const auto ids = s.placeSequence(shakyUid, 0, 0, 0, false);
            const Id nest = ids.empty() ? 0 : ids.front();
            keys = s.stabilizeClip(nest);
            if (const Clip* c = s.seq().clip(nest)) {
                const double zoom = c->motion.p("scale", 0) / 100;
                double prevRaw[2] = {0, 0}, prevLeft[2] = {0, 0};
                int n = 0;
                for (const auto& [t, xy] : shake) {
                    if (t < S / 2 || t > 3 * S + S / 2) continue;  // (the smoothing window is clamped at the ends)
                    const double raw[2] = {xy.first * zoom, xy.second * zoom};
                    const double left[2] = {raw[0] + c->motion.p("posX", t), raw[1] + c->motion.p("posY", t)};
                    if (n++) {
                        rawJitter += std::hypot(raw[0] - prevRaw[0], raw[1] - prevRaw[1]);
                        leftJitter += std::hypot(left[0] - prevLeft[0], left[1] - prevLeft[1]);
                    }
                    std::copy(raw, raw + 2, prevRaw);
                    std::copy(left, left + 2, prevLeft);
                }
            }
            s.deleteSequence(s.activeSequence());
            s.deleteSequence(s.activeSequence());
            s.switchSequence(0);
        }
        check(keys > 30 && rawJitter > 0 && leftJitter < 0.25 * rawJitter, "Stabilize removes most of the shake",
              "frame-to-frame motion " + std::to_string(leftJitter) + " px vs " + std::to_string(rawJitter) + " px before");
        QFile::remove(still);
    }
    // Export queue: two exports run one after the other in the background while the UI stays live
    {
        ui::ExportQueue* q = s.exportQueue();
        q->clearFinished();
        auto settings = [&](const QString& name) {
            ExportSettings es;
            es.path = (QDir::tempPath() + "/" + name).toStdString();
            es.width = 640;
            es.height = 360;
            es.rangeStart = 2 * S;
            es.rangeEnd = 3 * S;
            es.video.codec = "libx264";
            es.video.bitrate = 2'000'000;
            es.video.maxBitrate = 3'000'000;
            QFile::remove(QString::fromStdString(es.path));
            return es;
        };
        const int a = q->add(s.snapshot(true), settings("cutline_uitest_q1.mp4"), "Sequence 01");
        const int b = q->add(s.snapshot(true), settings("cutline_uitest_q2.mp4"), "Sequence 01");
        const bool bWaited = q->entry(b)->state == ui::ExportQueue::State::Waiting && q->entry(a)->state == ui::ExportQueue::State::Running;
        const bool flagged = s.exporting();
        QElapsedTimer qt;
        qt.start();
        while (q->busy() && qt.elapsed() < 60000) pump(50);
        const bool bothDone = q->entry(a)->state == ui::ExportQueue::State::Done && q->entry(b)->state == ui::ExportQueue::State::Done;
        const qint64 s1 = QFileInfo(QDir::tempPath() + "/cutline_uitest_q1.mp4").size(), s2 = QFileInfo(QDir::tempPath() + "/cutline_uitest_q2.mp4").size();
        check(bWaited && flagged && bothDone && !s.exporting() && s1 > 2000 && s2 > 2000,
              "export queue runs exports one after another in the background",
              "waited " + std::to_string(bWaited) + " flagged " + std::to_string(flagged) + " done " + std::to_string(bothDone) +
                  " exporting " + std::to_string(s.exporting()) + " sizes " + std::to_string(s1) + "/" + std::to_string(s2) + " | " +
                  q->entry(b)->detail.left(120).replace('\n', ' ').toStdString());
        q->clearFinished();
        check(q->entries().empty(), "Clear Finished empties the queue");
        QFile::remove(QDir::tempPath() + "/cutline_uitest_q1.mp4");
        QFile::remove(QDir::tempPath() + "/cutline_uitest_q2.mp4");
    }
    // Auto Color: a still with an orange cast gets a cooler white balance (rendered and analysed on the GPU)
    {
        QImage img(640, 360, QImage::Format_RGB32);
        std::mt19937 rng(4);
        std::uniform_int_distribution<int> u(30, 230);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const double v = u(rng) / 255.0;
                auto enc = [&](double gain) { return int(std::clamp(std::pow(std::pow(v, 2.2) * gain, 1 / 2.2), 0.0, 1.0) * 255); };
                img.setPixel(x, y, qRgb(enc(1.4), enc(1.0), enc(0.7)));
            }
        const QString still = QDir::tempPath() + "/cutline_uitest_warm.png";
        img.save(still);
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({still}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer at;
        at.start();
        while (!done && at.elapsed() < 20000) pump(20);
        double temp = 0;
        int n = 0;
        if (!imported.empty()) {
            const Asset a = *s.project().asset(imported.front());
            Id cid = 0;
            s.edit("place", [&](Project& p) {
                cid = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, a, 40 * S, 0, 2 * S, 1, 0)).front();
            });
            s.setSelection({cid}, false);
            s.setPlayhead(41 * S);
            n = s.autoColorSelection();
            if (const Clip* c = s.seq().clip(cid))
                for (const auto& e : c->effects)
                    if (e.type == fx::Color) temp = e.p("temperature", 0);
            s.undo();
            s.undo();
        }
        check(n == 1 && temp < -20, "Auto Color cools down a warm frame", "temperature " + std::to_string(temp));
        QFile::remove(still);
    }
    // Punch In Here: 1.5x toward a point; that point of the picture stays where it was
    {
        const Clip* v = on(s, TrackKind::Video, 0).front();
        const Id vid = v->id;
        s.setPlayhead(v->start + 2 * S);
        const int W = s.seq().settings.width, H = s.seq().settings.height;
        const double px = W * 0.8, py = H * 0.25;
        const bool ok = s.punchIn(vid, px, py, 1.5);
        const Clip* c = s.seq().clip(vid);
        auto motionAtT = [&](Tick local) {
            MotionValues mv;
            mv.posX = c->motion.p("posX", local);
            mv.posY = c->motion.p("posY", local);
            mv.scale = c->motion.p("scale", local);
            return mv;
        };
        // where the source point under (px, py) at the start shows up at the peak (bilinear corners)
        const Tick l0 = s.playhead() - c->start, l1 = l0 + S;
        const Corners a = computeCorners(W, H, W, H, c->fit, motionAtT(l0)), b = computeCorners(W, H, W, H, c->fit, motionAtT(l1));
        const double u = (px - a.p[0][0]) / (a.p[1][0] - a.p[0][0]), vv = (py - a.p[0][1]) / (a.p[2][1] - a.p[0][1]);
        const double qx = b.p[0][0] + u * (b.p[1][0] - b.p[0][0]), qy = b.p[0][1] + vv * (b.p[2][1] - b.p[0][1]);
        check(ok && c->motion.p("scale", l1) > 149 && std::hypot(qx - px, qy - py) < 1.0, "Punch In Here zooms toward the point and keeps it in place",
              "moved " + std::to_string(std::hypot(qx - px, qy - py)) + " px");
        s.undo();
    }
    // Effect Controls: right-click a keyframe lane > Copy Keyframes, then on another clip > Paste at Playhead
    {
        auto vclips = on(s, TrackKind::Video, 0);
        const Id from = vclips.front()->id, to = vclips.back()->id;
        s.edit("keys", [&](Project& p) {
            Param& px = p.sequence.clip(from)->motion.params["posX"];
            px.addKey(0, -300);
            px.addKey(S, 300, Interp::Ease);
        });
        auto menuPick = [&](const QString& text) {  // pick an item of the context menu once it is open
            QTimer::singleShot(150, [text] {
                if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                    for (QAction* a : m->actions())
                        if (a->text() == text && a->isEnabled()) a->trigger();
                    m->close();
                }
            });
        };
        auto laneOf = [&](const QString& paramLabel) -> ui::KeyLane* {
            pump(100);
            for (auto* l : w.findChildren<ui::KeyLane*>())
                if (l->property("paramLabel").toString() == paramLabel && l->isVisible()) return l;
            return nullptr;
        };
        if (auto* d = w.findChild<QDockWidget*>("fxcontrols")) d->raise();
        s.setSelection({from}, false);
        s.setPlayhead(s.seq().clip(from)->start);
        ui::KeyLane* l1 = laneOf("Position X");
        if (l1) {
            menuPick("Copy Keyframes");
            mouse(l1, QEvent::MouseButtonPress, QPoint(l1->width() - 4, l1->height() / 2), Qt::RightButton, Qt::RightButton, {});
            pump(300);
        }
        const Clip* target = s.seq().clip(to);
        s.setSelection({to}, false);
        s.setPlayhead(target->start + 2 * S);
        ui::KeyLane* l2 = laneOf("Position X");
        if (l2) {
            menuPick("Paste Keyframes at Playhead");
            mouse(l2, QEvent::MouseButtonPress, QPoint(l2->width() - 4, l2->height() / 2), Qt::RightButton, Qt::RightButton, {});
            pump(300);
        }
        const Param* got = nullptr;
        if (const Clip* c = s.seq().clip(to); c && c->motion.params.count("posX")) got = &c->motion.params.at("posX");
        const bool ok = l1 && l2 && got && got->keys.size() == 2 && got->keys[0].t == 2 * S && got->keys[1].t == 3 * S &&
                        got->keys[0].v == -300 && got->keys[1].interp == Interp::Ease;
        check(ok, "Effect Controls: copy keyframes from one clip and paste them at the playhead on another",
              got ? std::to_string(got->keys.size()) + " keys" : std::string("no lane"));
        if (ok) s.undo();
        s.undo();
    }
    // Clip > Replace With Source Clip: same place and length, the Source clip from its In point
    {
        const Clip* v = nullptr;
        for (const Clip* c : on(s, TrackKind::Video, 0))
            if (!v && c->kind == ClipKind::Media && c->speed == 1.0 && !c->reverse && c->duration < 30 * S) v = c;
        const Id vid = v->id;
        const Tick start = v->start, dur = v->duration;
        Id uw = 0;
        for (const Asset& a : s.project().assets)
            if (a.name == "uw_3440x1440p60_hevc.mkv") uw = a.id;
        s.openInSource(uw);
        s.sourceIn = 2 * S;
        s.setSelection({vid});
        const int n = s.replaceWithSourceClip();
        const Clip* c = s.seq().clip(vid);
        check(n >= 1 && c && c->assetId == uw && c->sourceIn == 2 * S && c->start == start && c->duration == dur,
              "Replace With Source Clip swaps the media in place", std::to_string(n) + " clips");
        s.sourceIn = kInvalidTick;
        s.undo();
    }
    // Effects > Looks: double-clicking a look puts a configured Color Correction on the selection
    {
        ui::EffectsPanel* ep = w.findChild<ui::EffectsPanel*>();
        QTreeWidget* tree = ep ? ep->findChild<QTreeWidget*>() : nullptr;
        QTreeWidgetItem* punchy = nullptr;
        if (tree)
            for (QTreeWidgetItem* it : tree->findItems("Punchy", Qt::MatchExactly | Qt::MatchRecursive)) punchy = it;
        const Clip* v = nullptr;
        for (const Clip* c : on(s, TrackKind::Video, 0))
            if (!v && c->kind == ClipKind::Media) v = c;
        const Id vid = v ? v->id : 0;
        s.setSelection({vid}, false);
        if (punchy) emit tree->itemDoubleClicked(punchy, 0);
        double sat = 0;
        if (const Clip* c = s.seq().clip(vid))
            for (const auto& e : c->effects)
                if (e.type == fx::Color) sat = e.p("saturation", 0);
        check(punchy && sat == 125, "Effects > Looks applies a colour look", "saturation " + std::to_string(sat));
        if (sat == 125) s.undo();
    }
    // Picture in Picture: bottom right at 30 % of the width, inset from the edges, rounded + shadow
    {
        const Id vid = on(s, TrackKind::Video, 0).front()->id;
        s.setSelection({vid}, false);
        const int n = s.pictureInPicture(3);
        const Clip* c = s.seq().clip(vid);
        const int W = s.seq().settings.width, H = s.seq().settings.height;
        MotionValues mv;
        mv.posX = c->motion.p("posX", 0);
        mv.posY = c->motion.p("posY", 0);
        mv.scale = c->motion.p("scale", 0);
        const Corners k = computeCorners(W, H, W, H, c->fit, mv);
        int nfx = 0;
        for (const auto& e : c->effects) nfx += e.type == fx::Rounded || e.type == fx::DropShadow;
        const double right = W - k.p[3][0], bottom = H - k.p[3][1], width = k.p[1][0] - k.p[0][0];
        check(n == 1 && std::abs(width - 0.3 * W) < 1 && right > 20 && right < 80 && std::abs(right - bottom) < 1 && nfx == 2,
              "Picture in Picture puts the clip in the corner at 30 % with rounded corners and a shadow",
              "width " + std::to_string(width) + " inset " + std::to_string(right) + "/" + std::to_string(bottom));
        s.undo();
    }
    // Fit Music to Video End: music under a video ends with it (looped or cut, fading out)
    {
        std::vector<Id> imported;
        bool done = false;
        s.importFiles({QString::fromStdString(g_media + "/music.mp3")}, 0, [&](const std::vector<Id>& ids) {
            imported = ids;
            done = true;
        });
        QElapsedTimer mt;
        mt.start();
        while (!done && mt.elapsed() < 20000) pump(20);
        Tick musicEnd = -1, fade = 0, videoEnd = 0;
        int pieces = 0;
        if (!imported.empty()) {
            const Asset music = *s.project().asset(imported.front());
            Id mainId = 0;
            for (const Asset& a : s.project().assets)
                if (a.name == "main_1440p60_h264.mkv") mainId = a.id;
            const Asset main = *s.project().asset(mainId);
            s.newSequence(s.seq().settings, "Music");
            Id mc = 0;
            s.edit("place", [&](Project& p) {
                edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, main, 0, 0, 3 * music.info.duration + 7 * S, 0, 0));
                mc = edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, music, 0, 0, music.info.duration, 0, 2, false, true)).front();
            });
            for (const auto& c : s.seq().clips)
                if (s.seq().track(c.trackId)->kind == TrackKind::Video) videoEnd = std::max(videoEnd, c.end());
            const Id musicTrack = s.seq().clip(mc)->trackId;
            s.setSelection({mc}, false);
            pieces = s.fitMusicToEnd();
            for (const Clip* c : s.seq().clipsOnTrack(musicTrack))
                if (c->end() > musicEnd) musicEnd = c->end(), fade = c->fadeOut;
            s.deleteSequence(s.activeSequence());
            s.switchSequence(0);
        }
        check(pieces >= 4 && musicEnd == videoEnd && fade == 3 * S, "Fit Music to Video End loops the music to the last frame",
              std::to_string(pieces) + " pieces, ends " + secs(musicEnd) + " vs video " + secs(videoEnd));
    }
    // Add Edits at Markers: every unlocked track is cut at each marker (fresh sequence)
    {
        Id mainId = 0;
        for (const Asset& a : s.project().assets)
            if (a.name == "main_1440p60_h264.mkv") mainId = a.id;
        const Asset main = *s.project().asset(mainId);
        s.newSequence(s.seq().settings, "Cuts");
        s.edit("place", [&](Project& p) {
            edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, main, 0, 0, 20 * S, 0, 0));
            edit::addMarker(p.sequence, 3 * S);
            edit::addMarker(p.sequence, 7 * S + S / 2);
        });
        const int n = s.addEditsAtMarkers();
        std::string starts;
        for (const Clip* c : on(s, TrackKind::Video, 0)) starts += secs(c->start) + " ";
        const auto vs = on(s, TrackKind::Video, 0), as = on(s, TrackKind::Audio, 0);
        check(n == 2 && vs.size() == 3 && as.size() == 3 && vs[1]->start == 3 * S && vs[2]->start == 7 * S + S / 2,
              "Add Edits at Markers cuts every track at each marker", starts);
        s.deleteSequence(s.activeSequence());
        s.switchSequence(0);
    }
    // Graphics > Templates: animated titles inside the frame, one undo step each
    {
        s.setPlayhead(2 * S);
        const int W = s.seq().settings.width, H = s.seq().settings.height;
        bool inside = true;
        size_t made = 0;
        for (int k = 0; k < 3; ++k) {
            const auto ids = s.addTitleTemplate(k);
            made += ids.size();
            for (Id id : ids) {
                const Clip* c = s.seq().clip(id);
                if (!c) {
                    inside = false;
                    continue;
                }
                Corners q;
                SnapshotPtr snap = s.snapshot();
                // fully on screen once the animation has settled (1.5 s in, or mid-clip for the countdown)
                const Tick at = std::min(c->start + S + S / 2, c->start + c->duration / 2);
                if (c->kind != ClipKind::Text || !FrameRenderer::clipCorners(*snap, *c, at, q)) inside = false;
                else
                    for (auto& pt : q.p) inside = inside && pt[0] >= 0 && pt[0] <= W && pt[1] >= 0 && pt[1] <= H;
            }
        }
        check(made == 5 && inside, "Graphics > Templates add on-screen animated titles", std::to_string(made) + " clips");
        s.setPlayhead(3 * S + S / 2);
        shot("templates");
        for (int k = 0; k < 3; ++k) s.undo();
    }
    // Instant replay: the 5 s before the playhead again at 50 %, with a REPLAY title, one undo step
    {
        const Tick before = s.seq().duration();
        const size_t undoBefore = s.doc().undo.position();
        s.setPlayhead(8 * S);
        const bool ok = s.insertInstantReplay(5, 0.5);
        const Clip* replayTitle = nullptr;
        const Clip* slow = nullptr;
        for (const auto& c : s.seq().clips) {
            if (c.kind == ClipKind::Text && c.text == "REPLAY") replayTitle = &c;
            if (c.start == 8 * S && c.speed == 0.5 && s.seq().track(c.trackId)->kind == TrackKind::Video) slow = &c;
        }
        check(ok && replayTitle && slow && replayTitle->start == 8 * S && replayTitle->duration == 10 * S && s.seq().duration() == before + 10 * S &&
                  slow->sourceIn == 3 * S && s.playhead() == 8 * S,
              "Insert Instant Replay repeats the last 5 s at 50 % under a REPLAY title",
              "duration " + secs(s.seq().duration()) + " (was " + secs(before) + ")");
        s.setPlayhead(9 * S);
        shot("instant_replay");
        s.undo();
        check(s.seq().duration() == before && s.doc().undo.position() == undoBefore, "undo removes the replay in one step");
    }
    // Create Vertical Version: a 1080x1920 tab showing this sequence (nested, Fill), one undo step
    {
        const Id srcUid = s.seq().uid;
        const Tick srcDur = s.seq().duration();
        const int tabs = int(s.project().otherSequences.size()) + 1;
        s.createVerticalVersion();
        pump(50);
        const Clip* vc = nullptr;
        for (const auto& c : s.seq().clips)
            if (c.kind == ClipKind::Nested && s.seq().track(c.trackId)->kind == TrackKind::Video) vc = &c;
        check(s.seq().settings.width == 1080 && s.seq().settings.height == 1920 && vc && vc->nestedSeq == srcUid &&
                  vc->fit == FitMode::Fill && vc->duration >= srcDur && int(s.project().otherSequences.size()) + 1 == tabs + 1,
              "Create Vertical Version nests the sequence into a 1080x1920 tab (Fill)",
              std::to_string(s.seq().settings.width) + "x" + std::to_string(s.seq().settings.height));
        s.undo();
        pump(50);
        check(int(s.project().otherSequences.size()) + 1 == tabs && s.seq().uid == srcUid, "undo removes the vertical version");
    }
    // Voice-over: records while the timeline plays, then the take lands on a free audio track lined
    // up with the picture. Under test the recorder captures the output device (WASAPI loopback of the
    // S/PDIF test device) - tests never open the microphone - so the take must match what played.
    if (qEnvironmentVariableIsSet("CUTLINE_AUDIO_DEVICE")) {
        qputenv("CUTLINE_RECORD_LOOPBACK", "1");
        const Asset* ba = nullptr;
        for (const Asset& a : s.project().assets)
            if (a.name == "beats.wav") ba = &a;
        if (ba) {
            const Asset beats = *ba;
            s.newSequence(s.seq().settings, "Voice-over");
            s.edit("place", [&](Project& p) {
                edit::overwriteClips(p.sequence, edit::clipsForAsset(p.sequence, beats, 0, 0, std::min<Tick>(beats.info.duration, 8 * S), 0, 0));
            });
            s.setPlayhead(1 * S);
            pump(200);
            QString verr;
            const bool started = s.startVoiceover(verr);
            QElapsedTimer vt;
            vt.start();
            while (vt.elapsed() < 3000) pump(20);
            const double recSec = s.voiceoverSeconds();
            s.togglePlay(s.programViewer());  // Space: stops playback and the take
            vt.restart();
            while (s.lastVoiceoverClip() == 0 && vt.elapsed() < 20000) pump(20);
            const Clip* vo = s.lastVoiceoverClip() ? s.seq().clip(s.lastVoiceoverClip()) : nullptr;
            const Track* voTrack = vo ? s.seq().track(vo->trackId) : nullptr;
            check(started && vo && voTrack && s.seq().trackIndexInKind(vo->trackId) == 1 && vo->start >= 1 * S &&
                      vo->start < 1 * S + S / 2 && vo->duration > 2 * S && vo->duration < 4 * S,
                  "Record Voice-over places the take on the next free audio track at the playhead",
                  verr.toStdString() + " rec " + std::to_string(recSec) + " s" +
                      (vo ? " at " + secs(vo->start) + " for " + secs(vo->duration) + " on A" + std::to_string(s.seq().trackIndexInKind(vo->trackId) + 1) : std::string()));
            // the take lines up with what was playing: envelope cross-correlation (1 ms bins)
            if (vo) {
                const Asset* va = s.project().asset(vo->assetId);
                AudioReader rec, src;
                std::string e1, e2;
                int bestLag = 9999;
                double bestScore = 0, zeroScore = 0;
                if (va && rec.open(va->path, 0, 0, e1) && src.open(beats.path, 0, 0, e2)) {
                    const int n = int(std::min<Tick>(vo->duration, 2 * S) * 48000 / kTicksPerSecond);
                    const int pad = 4800;  // +-100 ms
                    std::vector<float> a(size_t(n) * 2), b(size_t(n + 2 * pad) * 2);
                    rec.read(ticksToSamples(vo->sourceIn, 48000), a.data(), n);
                    src.read(ticksToSamples(vo->start, 48000) - pad, b.data(), n + 2 * pad);
                    auto env = [](const std::vector<float>& v) {
                        std::vector<double> e(v.size() / 96, 0.0);
                        for (size_t i = 0; i < e.size(); ++i)
                            for (size_t k = 0; k < 96; ++k) e[i] += std::abs(v[i * 96 + k]);
                        return e;
                    };
                    const auto ea = env(a), eb = env(b);
                    for (int lag = -100; lag <= 100; ++lag) {
                        double sc = 0;
                        for (size_t i = 0; i < ea.size(); ++i) sc += ea[i] * eb[size_t(int(i) + 100 + lag)];
                        if (sc > bestScore) bestScore = sc, bestLag = lag;
                        if (lag == 0) zeroScore = sc;
                    }
                }
                check(std::abs(bestLag) <= 15 && bestScore > 0, "the voice-over take is in sync with the timeline (within 15 ms)",
                      "lag " + std::to_string(bestLag) + " ms, score " + std::to_string(bestScore) + " (at 0: " + std::to_string(zeroScore) + ") " + e1 + e2);
                if (va) QFile::remove(QString::fromStdString(va->path));
            }
            s.deleteSequence(s.activeSequence());
            s.switchSequence(0);
        }
        qunsetenv("CUTLINE_RECORD_LOOPBACK");
    }
    // Saved shortcut overrides are applied to the commands
    {
        QSettings().setValue("shortcuts/cmd.test.cmd", "Ctrl+Alt+Shift+Q");
        QAction a;
        a.setObjectName("cmd.test.cmd");
        a.setShortcut(QKeySequence("Ctrl+1"));
        ui::applyShortcutOverrides({&a});
        check(a.shortcut() == QKeySequence("Ctrl+Alt+Shift+Q") && a.property("defaultShortcut").value<QKeySequence>() == QKeySequence("Ctrl+1"),
              "custom shortcuts override defaults (default kept for reset)");
        QSettings().remove("shortcuts/cmd.test.cmd");
    }
    s.doc().markSaved();  // no save prompt on close
    w.close();
    pump(100);
}
// Model invariants (same as the core fuzzer): no overlaps, frame aligned, sources inside media.
std::string invariants(const Session& s) {
    const Sequence& q = s.seq();
    const Tick F = q.frameDur();
    std::map<Id, std::vector<const Clip*>> byTrack;
    std::set<Id> ids;
    for (const auto& c : q.clips) {
        if (!ids.insert(c.id).second || !c.id) return "duplicate or missing clip id " + std::to_string(c.id);
        const Track* tr = q.track(c.trackId);
        if (!tr) return "clip on a missing track";
        if (c.duration <= 0 || c.start < 0) return "clip " + std::to_string(c.id) + " has a bad range";
        if (c.start % F || c.duration % F) return "clip " + std::to_string(c.id) + " not frame aligned";
        if (c.kind == ClipKind::Media) {
            const Asset* a = s.project().asset(c.assetId);
            if (!a) return "clip on a missing asset";
            if (!c.isHold() && !a->info.isImage && (c.sourceIn < 0 || c.sourceOut() > a->info.duration + F))
                return "clip " + std::to_string(c.id) + " reads outside its media";
        }
        byTrack[c.trackId].push_back(&c);
    }
    for (auto& [tid, cs] : byTrack) {
        std::sort(cs.begin(), cs.end(), [](const Clip* x, const Clip* y) { return x->start < y->start; });
        for (size_t i = 1; i < cs.size(); ++i)
            if (cs[i - 1]->end() > cs[i]->start)
                return "clips " + std::to_string(cs[i - 1]->id) + " and " + std::to_string(cs[i]->id) + " overlap";
    }
    for (const auto& tr : q.transitions) {
        const Clip* a = tr.clipA ? q.clip(tr.clipA) : nullptr;
        const Clip* b = tr.clipB ? q.clip(tr.clipB) : nullptr;
        if ((tr.clipA && !a) || (tr.clipB && !b)) return "transition on a missing clip";
        if (a && b && a->end() != b->start) return "transition between clips that do not touch";
    }
    return {};
}

// Random mouse / keyboard / command input on a real timeline; invariants after every action.
void monkeyTests(int iterations, unsigned seed, bool window) {
    std::printf("--- monkey%s: %d actions, seed %u ---\n", window ? " (main window + engine)" : "", iterations, seed);
    Session s;
    QString err;
    std::unique_ptr<ui::MainWindow> mw;
    std::unique_ptr<ui::TimelineWidget> own;
    if (window) {
        if (!s.startEngine(err)) {
            check(false, "engine start", err.toStdString());
            return;
        }
        mw = std::make_unique<ui::MainWindow>(&s);
        mw->resize(1600, 950);
        mw->show();
        pump(300);
    }
    if (!s.openProject(QString::fromStdString(makeProject()), err)) {
        check(false, "open project", err.toStdString());
        return;
    }
    if (!window) {
        own = std::make_unique<ui::TimelineWidget>(&s);
        own->resize(1600, 600);
        own->show();
    }
    ui::TimelineWidget& tl = window ? *mw->findChild<ui::TimelineWidget*>() : *own;
    pump(window ? 800 : 50);
    tl.zoomToFit();
    const Sequence opened = s.seq();
    std::mt19937 rng(seed);
    auto ir = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const Qt::KeyboardModifiers mods[] = {Qt::NoModifier, Qt::NoModifier, Qt::NoModifier, Qt::ControlModifier,
                                          Qt::AltModifier, Qt::ShiftModifier};
    const Session::Tool tools[] = {Session::Tool::Select, Session::Tool::Select, Session::Tool::Select, Session::Tool::Razor,
                                   Session::Tool::Ripple, Session::Tool::Hand, Session::Tool::Zoom, Session::Tool::RateStretch};
    std::string last;
    int actions = 0;
    const bool verify = qEnvironmentVariableIsSet("CUTLINE_MONKEY_VERIFY");
    for (int i = 0; i < iterations; ++i) {
        const int kind = ir(0, 9);
        const QPoint p(ir(0, tl.width() - 1), ir(0, tl.height() - 1));
        const Qt::KeyboardModifiers m = mods[ir(0, 5)];
        if (kind <= 3) {  // drag (most edits come from drags)
            last = "drag";
            const QPoint to = p + QPoint(ir(-300, 300), ir(-120, 120));
            mouse(&tl, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, m);
            const int steps = ir(1, 10);
            for (int k = 1; k <= steps; ++k) mouse(&tl, QEvent::MouseMove, p + (to - p) * k / steps, Qt::NoButton, Qt::LeftButton, m);
            mouse(&tl, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, m);
        } else if (kind == 4) {
            last = "click";
            mouse(&tl, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, m);
            mouse(&tl, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, m);
        } else if (kind == 5) {
            last = "tool";
            s.setTool(tools[ir(0, 7)]);
        } else if (kind == 6) {
            last = "wheel";
            QWheelEvent we(QPointF(p), tl.mapToGlobal(QPointF(p)), {}, QPoint(0, ir(0, 1) ? 120 : -120), Qt::NoButton, m,
                           Qt::NoScrollPhase, false);
            QApplication::sendEvent(&tl, &we);
        } else if (kind == 7) {
            static const int keys[] = {Qt::Key_Delete, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down, Qt::Key_K,
                                       Qt::Key_Z, Qt::Key_C, Qt::Key_V, Qt::Key_Home, Qt::Key_End, Qt::Key_D};
            const int k = keys[ir(0, 11)];
            last = "key " + std::to_string(k);
            key(&tl, k, m);
        } else {
            // session commands the menus run
            s.setPlayhead(s.snapToFrame(Tick(ir(0, 40)) * S / 2));
            int cmd = ir(0, window ? 35 : 20);
            if (!window && cmd >= 16) {  // offscreen runs skip playback / sequence commands
                static const int extra[] = {27, 29, 30, 33, 34};
                cmd = extra[cmd - 16];
            }
            last = "command " + std::to_string(cmd);
            switch (cmd) {
            case 0: s.undo(); break;
            case 1: s.redo(); break;
            case 2: s.deleteSelection(ir(0, 1)); break;
            case 3: s.addEditAtPlayhead(ir(0, 1)); break;
            case 4: s.copySelection(); break;
            case 5: s.paste(ir(0, 1)); break;
            case 6: s.rippleTrimToPlayhead(ir(0, 1)); break;
            case 7: s.closeAllGaps(); break;
            case 8: s.nudge(ir(-5, 5)); break;
            case 9: s.applyDefaultTransition(ir(0, 1)); break;
            case 10: s.addFrameHold(ir(0, 1)); break;
            case 11: s.toggleLink(); break;
            case 12: s.selectClipAtPlayhead(); break;
            case 13:
                s.edit("in/out", [&](Project& p) {
                    p.sequence.inPoint = s.snapToFrame(Tick(ir(0, 20)) * S);
                    p.sequence.outPoint = p.sequence.inPoint + s.snapToFrame(Tick(ir(1, 10)) * S);
                });
                s.liftExtract(ir(0, 1));
                break;
            case 14: s.selectAll(); break;
            case 15: s.toggleEnable(); break;
            case 16: s.togglePlay(ir(0, 3) ? s.programViewer() : s.sourceViewer()); break;
            case 17: s.shuttle(s.programViewer(), ir(-1, 1)); break;
            case 18: s.scrubPlayhead(s.snapToFrame(Tick(ir(0, 60)) * S / 2)); break;
            case 19: s.openInSource(s.project().assets[size_t(ir(0, int(s.project().assets.size()) - 1))].id); break;
            case 20: {
                const char* docks[] = {"scopes", "source", "fxcontrols", "mixer", "history", "markers", "project", "effects"};
                if (auto* d = mw->findChild<QDockWidget*>(docks[ir(0, 7)])) d->raise();
                break;
            }
            case 21: pump(ir(20, 300)); break;  // let playback run a while
            case 22: if (s.sequenceNames().size() < 4) s.duplicateSequence(); break;
            case 23: s.switchSequence(ir(0, int(s.sequenceNames().size()) - 1)); break;
            case 24: if (s.sequenceNames().size() > 1) s.deleteSequence(ir(0, int(s.sequenceNames().size()) - 1)); break;
            case 25: if (s.sequenceNames().size() < 5) s.nestSelection(); break;
            case 26: if (Id pc = s.primaryClip()) s.openNested(pc); break;
            case 27: s.synchronizeSelection(); break;
            case 28: if (s.sequenceNames().size() < 5) s.createVerticalVersion(); break;
            case 29: s.addHighlightMarkers(); break;
            case 30: s.pictureInPicture(ir(0, 4)); break;
            case 31: s.autoColorSelection(); break;
            case 32: s.insertInstantReplay(ir(1, 6), ir(0, 1) ? 0.5 : 0.25); break;
            case 33: s.fitMusicToEnd(); break;
            case 34: s.punchIn(s.primaryClip(), ir(0, 2560), ir(0, 1440), ir(0, 1) ? 1.5 : 2.0); break;
            case 35: s.trackMaskForward(s.primaryClip()); break;
            }
        }
        pump();
        ++actions;
        std::string bad = invariants(s);
        if (bad.empty() && verify && !s.dragging()) {
            // the whole history must still lead back to the opened project and forward again
            const Sequence now = s.seq();
            Document& d = s.doc();
            const size_t pos = d.undo.position();
            while (d.undo.canUndo()) d.doUndo();
            const bool back = d.project.sequence == opened;
            while (d.undo.position() < pos && d.undo.canRedo()) d.doRedo();
            if (!back) bad = "undo-all does not reach the opened project (last step: " + d.undo.undoLabel() + ")";
            else if (!(d.project.sequence == now)) bad = "redo-all does not reproduce the current state";
        }
        if (!bad.empty()) {
            check(false, "monkey: model stays valid", "after action " + std::to_string(i) + " (" + last + "): " + bad);
            return;
        }
    }
    // drags always end; then everything must undo back to the opened project
    if (s.dragging()) s.endDrag(true);
    s.stop();
    while (s.doc().undo.canUndo()) s.undo();
    check(invariants(s).empty() && s.seq() == opened, "monkey: " + std::to_string(actions) + " random actions, model valid, undo-all ok",
          std::to_string(s.seq().clips.size()) + " clips after undo-all");
    s.doc().markSaved();
    mw.reset();
    s.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "--window") {
        // timeline part runs offscreen in a child process-free way
    }
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    qputenv("CUTLINE_TEST", "1");
    if (mode == "--timeline" || mode == "--monkey") qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("CutlineUiTest");
    ui::applyTheme(app);
    if (mode == "--monkey") {
        monkeyTests(argc > 2 ? std::atoi(argv[2]) : 3000, argc > 3 ? unsigned(std::atoi(argv[3])) : 1u, false);
        std::printf("\nuitest: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (mode == "--monkey-window") {
        monkeyTests(argc > 2 ? std::atoi(argv[2]) : 2000, argc > 3 ? unsigned(std::atoi(argv[3])) : 1u, true);
        std::printf("\nuitest: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (mode != "--window") timelineTests();
    if (mode != "--timeline") windowTests();
    std::printf("\nuitest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
