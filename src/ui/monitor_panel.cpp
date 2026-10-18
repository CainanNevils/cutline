#include "ui/monitor_panel.h"

#include "media/probe.h"
#include "ui/session.h"
#include "ui/theme.h"
#include "ui/video_widget.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <QDrag>
#include <QMimeData>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace cut::ui {

// Starts a drag of the Source monitor's In-Out range (picked up by the timeline).
class DragHandle : public QToolButton {
public:
    DragHandle(Session* s, bool video, bool audio, QWidget* parent) : QToolButton(parent), s_(s), video_(video), audio_(audio) {
        setAutoRaise(true);
        setIconSize({18, 18});
        setCursor(Qt::OpenHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override { press_ = e->position(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || (e->position() - press_).manhattanLength() < 6 || !s_->sourceAsset()) return;
        Tick in = s_->sourceIn != kInvalidTick ? s_->sourceIn : 0;
        Tick out = s_->sourceOut != kInvalidTick ? s_->sourceOut : s_->sourceDuration();
        auto* md = new QMimeData;
        md->setData("application/x-cutline-source", QString("%1;%2;%3;%4;%5").arg(s_->sourceAsset()).arg(in).arg(out)
                                                        .arg(video_ ? 1 : 0).arg(audio_ ? 1 : 0).toUtf8());
        auto* drag = new QDrag(this);
        drag->setMimeData(md);
        drag->setPixmap(icon().pixmap(24, 24));
        drag->exec(Qt::CopyAction);
    }

private:
    Session* s_;
    bool video_, audio_;
    QPointF press_;
};

// ---------------------------------------------------------------------------------------------
ScrubBar::ScrubBar(Session* s, bool program, QWidget* parent) : QWidget(parent), s_(s), program_(program) {
    setMinimumHeight(22);
    setMaximumHeight(22);
    setCursor(Qt::PointingHandCursor);
    connect(s_, program_ ? &Session::playheadChanged : &Session::sourcePositionChanged, this,
            [this] { update(); });
    connect(s_, &Session::changed, this, [this] { update(); });
    connect(s_, &Session::sourceChanged, this, [this] { update(); });
}

Tick ScrubBar::duration() const {
    return program_ ? std::max<Tick>(s_->seq().duration(), kTicksPerSecond) : s_->sourceDuration();
}
Tick ScrubBar::position() const { return program_ ? s_->playhead() : s_->sourcePosition(); }

void ScrubBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), col::bg);
    const Tick d = duration();
    if (d <= 0) return;
    const double w = width() - 12;
    auto x = [&](Tick t) { return 6 + w * double(t) / double(d); };
    Tick in = program_ ? s_->seq().inPoint : s_->sourceIn;
    Tick out = program_ ? s_->seq().outPoint : s_->sourceOut;
    if (in != kInvalidTick || out != kInvalidTick) {
        double a = x(in == kInvalidTick ? 0 : in), b = x(out == kInvalidTick ? d : out);
        p.fillRect(QRectF(a, 4, b - a, height() - 8), QColor(0x2d, 0x8c, 0xeb, 90));
    }
    p.setPen(QColor(0x50, 0x50, 0x50));
    p.drawLine(6, height() / 2, width() - 6, height() / 2);
    if (program_) {
        p.setPen(Qt::NoPen);
        p.setBrush(col::marker);
        for (const auto& m : s_->seq().markers) p.drawRect(QRectF(x(m.t) - 1.5, 3, 3, 6));
    }
    double px = x(position());
    p.setPen(QPen(col::playhead, 2));
    p.drawLine(QPointF(px, 2), QPointF(px, height() - 2));
    p.setBrush(col::playhead);
    p.setPen(Qt::NoPen);
    QPolygonF tri;
    tri << QPointF(px - 5, 2) << QPointF(px + 5, 2) << QPointF(px, 8);
    p.drawPolygon(tri);
}

void ScrubBar::seekTo(int xp) {
    const Tick d = duration();
    double f = std::clamp((xp - 6.0) / std::max(1.0, width() - 12.0), 0.0, 1.0);
    Tick t = Tick(f * double(d));
    if (program_) s_->scrubPlayhead(t);
    else s_->scrubSource(t);
}

void ScrubBar::mousePressEvent(QMouseEvent* e) { seekTo(int(e->position().x())); }
void ScrubBar::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) seekTo(int(e->position().x()));
}

// ---------------------------------------------------------------------------------------------
namespace {
QToolButton* tb(const QString& iconName, const QString& tip, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize({18, 18});
    return b;
}
} // namespace

MonitorPanel::MonitorPanel(Session* s, bool program, QWidget* parent) : QWidget(parent), s_(s), program_(program) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    video_ = new VideoWidget(s, program ? s->programViewer() : s->sourceViewer(), program, this);
    v->addWidget(video_, 1);
    scrub_ = new ScrubBar(s, program, this);
    v->addWidget(scrub_);

    auto* bar = new QWidget(this);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(6, 2, 6, 4);
    h->setSpacing(2);
    tc_ = new QLineEdit(bar);
    tc_->setFixedWidth(118);
    tc_->setStyleSheet("QLineEdit { color: #4d9dff; font: bold 11pt 'Consolas'; background: transparent; border: none; }");
    tc_->setToolTip(tr("Current time - type a timecode and press Enter"));
    connect(tc_, &QLineEdit::returnPressed, this, [this] {
        Tick t;
        const QString text = tc_->text().trimmed();
        if (parseTimecode(toS(text), fps(), t)) {
            if (text.startsWith('+') || text.startsWith('-')) t = std::max<Tick>(0, position() + t);  // relative
            if (program_) s_->setPlayhead(t);
            else s_->setSourcePosition(t);
        }
        updateTimecode();
        video_->setFocus();
    });
    h->addWidget(tc_);
    h->addStretch(1);
    auto* mi = tb("markin", tr("Mark In (I)"), bar);
    auto* mo = tb("markout", tr("Mark Out (O)"), bar);
    auto* gi = tb("gotoin", tr("Go to In (Shift+I)"), bar);
    auto* sb = tb("stepback", tr("Step Back 1 Frame (Left)"), bar);
    play_ = tb("play", tr("Play/Stop (Space)"), bar);
    play_->setIconSize({22, 22});
    auto* sf = tb("stepfwd", tr("Step Forward 1 Frame (Right)"), bar);
    auto* go = tb("gotoout", tr("Go to Out (Shift+O)"), bar);
    for (auto* b : {mi, mo, gi, sb, play_, sf, go}) h->addWidget(b);
    connect(mi, &QToolButton::clicked, this, &MonitorPanel::markIn);
    connect(mo, &QToolButton::clicked, this, &MonitorPanel::markOut);
    connect(gi, &QToolButton::clicked, this, &MonitorPanel::goIn);
    connect(go, &QToolButton::clicked, this, &MonitorPanel::goOut);
    connect(sb, &QToolButton::clicked, this, [this] { step(-1); });
    connect(sf, &QToolButton::clicked, this, [this] { step(1); });
    connect(play_, &QToolButton::clicked, this,
            [this] { s_->togglePlay(program_ ? s_->programViewer() : s_->sourceViewer()); });
    if (!program_) {
        h->addSpacing(8);
        auto* ins = tb("insert", tr("Insert (,)"), bar);
        auto* ovr = tb("overwrite", tr("Overwrite (.)"), bar);
        h->addWidget(ins);
        h->addWidget(ovr);
        connect(ins, &QToolButton::clicked, this, [this] { s_->placeFromSource(true); });
        // drag handles: drag the In-Out range onto the timeline (both / video only / audio only)
        for (auto [ic, tip, vid, aud] : {std::tuple{"film", tr("Drag video and audio"), true, true},
                                      {"image", tr("Drag video only"), true, false},
                                      {"wave", tr("Drag audio only"), false, true}}) {
            auto* d = new DragHandle(s_, vid, aud, bar);
            d->setIcon(icon(ic));
            d->setToolTip(tip);
            h->addWidget(d);
        }
        connect(ovr, &QToolButton::clicked, this, [this] { s_->placeFromSource(false); });
    }
    if (program_) {
        h->addSpacing(8);
        auto* lift = tb("lift", tr("Lift (;) - remove In to Out, leave a gap"), bar);
        auto* extract = tb("extract", tr("Extract (') - remove In to Out and close the gap"), bar);
        auto* cam = tb("camera", tr("Export Frame (Ctrl+Shift+E)"), bar);
        h->addWidget(lift);
        h->addWidget(extract);
        h->addWidget(cam);
        connect(lift, &QToolButton::clicked, this, [this] { s_->liftExtract(false); });
        connect(extract, &QToolButton::clicked, this, [this] { s_->liftExtract(true); });
        connect(cam, &QToolButton::clicked, this, [this] { emit exportFrameRequested(); });
    }
    h->addStretch(1);
    durLabel_ = new QLabel(bar);
    durLabel_->setStyleSheet("color: #8c8c8c; font: 9pt 'Consolas';");
    h->addWidget(durLabel_);
    h->addSpacing(6);
    zoom_ = new QComboBox(bar);
    zoom_->addItems({tr("Fit"), "25%", "50%", "75%", "100%", "200%"});
    zoom_->setToolTip(tr("Zoom (Ctrl+wheel on the video)"));
    connect(zoom_, &QComboBox::currentIndexChanged, this, [this] { applyZoom(); });
    h->addWidget(zoom_);
    if (program_) {
        quality_ = new QComboBox(bar);
        quality_->addItems({tr("Auto"), tr("Full"), "1/2", "1/4"});
        quality_->setToolTip(tr("Playback resolution (Auto steps down when rendering cannot keep up)"));
        connect(quality_, &QComboBox::currentIndexChanged, this, [this](int i) {
            ViewSettings vs = video_->view();
            const PreviewQuality q[] = {PreviewQuality::Auto, PreviewQuality::Full, PreviewQuality::Half,
                                        PreviewQuality::Quarter};
            vs.quality = q[i];
            video_->setView(vs);
        });
        h->addWidget(quality_);
        proxy_ = tb("proxy", tr("Toggle Proxies (preview only; export always uses originals)"), bar);
        proxy_->setCheckable(true);
        connect(proxy_, &QToolButton::toggled, this, [this](bool on) { s_->setProxiesEnabled(on); });
        h->addWidget(proxy_);
    }
    safe_ = tb("safe", tr("Safe Margins"), bar);
    safe_->setCheckable(true);
    connect(safe_, &QToolButton::toggled, this, [this](bool on) {
        ViewSettings vs = video_->view();
        vs.safeMargins = on;
        video_->setView(vs);
    });
    h->addWidget(safe_);
    v->addWidget(bar);

    connect(video_, &VideoWidget::zoomChanged, this, [this] {
        zoom_->blockSignals(true);
        zoom_->setCurrentIndex(-1);
        zoom_->setEditable(true);
        zoom_->setEditText(QString::number(int(video_->view().zoom * 100)) + "%");
        zoom_->setEditable(false);
        zoom_->blockSignals(false);
    });
    auto upd = [this] { updateTimecode(); };
    connect(s_, program_ ? &Session::playheadChanged : &Session::sourcePositionChanged, this, upd);
    connect(s_, &Session::changed, this, upd);
    connect(s_, &Session::sourceChanged, this, upd);
    connect(s_, &Session::playStateChanged, this, [this](bool p) {
        bool mine = p && s_->engine() && s_->engine()->playingViewer() == (program_ ? s_->programViewer() : s_->sourceViewer());
        play_->setIcon(icon(mine ? "pause" : "play"));
    });
    setAcceptDrops(!program_);
    connect(s_, &Session::engineRestarted, this, &MonitorPanel::updateHint);
    if (!program_) {
        connect(s_, &Session::sourceChanged, this, [this] {
            waveComplete_ = false;
            updateAudioImage();
        });
        connect(s_, &Session::mediaUpdated, this, &MonitorPanel::updateAudioImage);
        connect(s_, &Session::engineRestarted, this, [this] {
            waveShown_ = waveComplete_ = false;
            updateAudioImage();
        });
    }
    QTimer::singleShot(0, this, &MonitorPanel::updateHint);
    hudTimer_ = new QTimer(this);
    connect(hudTimer_, &QTimer::timeout, this, &MonitorPanel::updateHud);
    updateTimecode();
}

void MonitorPanel::updateHint() {
    if (!s_->engine()) return;
    const QString text = program_ ? tr("Edit in the Timeline to see the result here")
                                  : tr("Double-click a clip in the Project panel\nor drop a file here to view it");
    QFont f("Segoe UI", 11);
    QFontMetrics fm(f);
    const QStringList lines = text.split('\n');
    int w = 0;
    for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    const double dpr = video_->devicePixelRatioF();
    QImage img(int((w + 20) * dpr), int((lines.size() * fm.height() + 12) * dpr), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setFont(f);
    p.setPen(QColor(0x8c, 0x8c, 0x8c));
    for (int i = 0; i < lines.size(); ++i)
        p.drawText(QRectF(0, 6 + i * fm.height(), w + 20, fm.height()), Qt::AlignCenter, lines[i]);
    p.end();
    std::vector<uint8_t> px(size_t(img.sizeInBytes()));
    std::memcpy(px.data(), img.constBits(), px.size());
    s_->engine()->setHintImage(program_ ? s_->programViewer() : s_->sourceViewer(), img.width(), img.height(), std::move(px));
}

void MonitorPanel::updateAudioImage() {
    if (program_ || !s_->engine()) return;
    const Asset* a = s_->project().asset(s_->sourceAsset());
    WaveformPtr wf = a && !a->info.hasVideo && !a->info.audio.empty() ? s_->waveform(a->id, 0) : nullptr;
    if (!wf) {
        if (waveShown_) s_->engine()->setAudioImage(s_->sourceViewer(), 0, 0, {});
        waveShown_ = false;
        waveComplete_ = false;
        return;
    }
    if (waveShown_ && waveComplete_) return;  // final image already sent
    const int W = 1600, H = 360;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.fillRect(QRect(0, H / 2, W, 1), QColor(255, 255, 255, 40));
    const int64_t total = ticksToSamples(a->info.duration, 48000);
    const QColor wave = col::audio.lighter(130);
    p.setPen(wave);
    for (int x = 0; x < W; ++x) {
        float mn = 0, mx = 0;
        if (!wf->range(total * x / W, std::max(total * (x + 1) / W, total * x / W + 1), mn, mx)) continue;
        p.drawLine(x, int(H / 2 - mx * (H / 2 - 4)), x, int(H / 2 - mn * (H / 2 - 4)));
    }
    p.end();
    std::vector<uint8_t> px(size_t(img.sizeInBytes()));
    std::memcpy(px.data(), img.constBits(), px.size());
    s_->engine()->setAudioImage(s_->sourceViewer(), W, H, std::move(px));
    waveShown_ = true;
    waveComplete_ = wf->complete.load();
}

void MonitorPanel::dragEnterEvent(QDragEnterEvent* e) {
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && isSupportedExtension(toS(u.toLocalFile()))) {
            e->acceptProposedAction();
            return;
        }
}

void MonitorPanel::dropEvent(QDropEvent* e) {
    QStringList files;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && isSupportedExtension(toS(u.toLocalFile()))) files << u.toLocalFile();
    if (files.isEmpty()) return;
    s_->importFiles(files, 0, [this](const std::vector<Id>& ids) {
        if (!ids.empty()) s_->openInSource(ids.front());
    });
    e->acceptProposedAction();
}

Rational MonitorPanel::fps() const { return program_ ? s_->seq().settings.fps : s_->sourceFps(); }
Tick MonitorPanel::position() const { return program_ ? s_->playhead() : s_->sourcePosition(); }

void MonitorPanel::updateTimecode() {
    if (!tc_->hasFocus()) tc_->setText(toQ(formatTimecode(position(), fps())));
    Tick in = program_ ? s_->seq().inPoint : s_->sourceIn;
    Tick out = program_ ? s_->seq().outPoint : s_->sourceOut;
    Tick dur = program_ ? s_->seq().duration() : s_->sourceDuration();
    Tick a = in == kInvalidTick ? 0 : in, b = out == kInvalidTick ? dur : out;
    durLabel_->setText(toQ(formatTimecode(std::max<Tick>(0, b - a), fps())));
}

void MonitorPanel::applyZoom() {
    ViewSettings vs = video_->view();
    const double z[] = {0, 0.25, 0.5, 0.75, 1.0, 2.0};
    int i = zoom_->currentIndex();
    if (i < 0) return;
    vs.zoom = z[i] * video_->devicePixelRatioF();
    vs.panX = vs.panY = 0;
    video_->setView(vs);
}

void MonitorPanel::markIn() {
    if (program_) s_->edit(tr("Mark In"), [&](Project& p) { p.sequence.inPoint = s_->playhead(); });
    else {
        s_->sourceIn = s_->sourcePosition();
        emit s_->changed();
    }
}

void MonitorPanel::markOut() {
    const Tick fd = frameDuration(fps());
    if (program_) s_->edit(tr("Mark Out"), [&](Project& p) { p.sequence.outPoint = s_->playhead() + fd; });
    else {
        s_->sourceOut = s_->sourcePosition() + fd;
        emit s_->changed();
    }
}

void MonitorPanel::goIn() {
    Tick in = program_ ? s_->seq().inPoint : s_->sourceIn;
    if (in == kInvalidTick) in = 0;
    if (program_) s_->setPlayhead(in);
    else s_->setSourcePosition(in);
}

void MonitorPanel::goOut() {
    const Tick fd = frameDuration(fps());
    Tick out = program_ ? s_->seq().outPoint : s_->sourceOut;
    Tick dur = program_ ? s_->seq().duration() : s_->sourceDuration();
    Tick t = (out == kInvalidTick ? dur : out) - fd;
    if (program_) s_->setPlayhead(std::max<Tick>(0, t));
    else s_->setSourcePosition(std::max<Tick>(0, t));
}

void MonitorPanel::step(int frames) {
    s_->stop();
    const Tick fd = frameDuration(fps());
    Tick t = std::max<Tick>(0, position() + fd * frames);
    if (program_) s_->scrubPlayhead(t);
    else s_->scrubSource(t);
}

void MonitorPanel::setHud(bool on) {
    hud_ = on;
    ViewSettings vs = video_->view();
    vs.showHud = on;
    video_->setView(vs);
    if (on) {
        updateHud();
        hudTimer_->start(250);
    } else hudTimer_->stop();
}

void MonitorPanel::updateHud() {
    PlaybackEngine* e = s_->engine();
    if (!e) return;
    const int viewer = program_ ? s_->programViewer() : s_->sourceViewer();
    ViewerStats st = e->stats(viewer);
    AudioStatus au = e->audioStatus();
    QStringList lines;
    lines << QString("%1 fps   dropped %2   late %3").arg(st.fps, 0, 'f', 0).arg(st.dropped).arg(st.lateFrames);
    lines << QString("render %1 ms   GPU %2 ms   preview %3")
                 .arg(st.renderMs, 0, 'f', 2)
                 .arg(st.gpuMs, 0, 'f', 2)
                 .arg(st.previewDivisor == 1 ? QString("Full") : QString("1/%1").arg(st.previewDivisor));
    lines << QString("decode: %1 GPU / %2 CPU%3   seek %4 ms")
                 .arg(st.hwSources)
                 .arg(st.swSources)
                 .arg(st.swSources ? "  (software fallback)" : "")
                 .arg(st.lastSeekMs, 0, 'f', 0);
    auto gpu = e->gpu();
    lines << QString("VRAM %1 MB   frames %2 MB")
                 .arg(gpu ? double(gpu->vramUsage()) / 1048576.0 : 0, 0, 'f', 0)
                 .arg(gpu ? double(gpu->textureBytesInUse()) / 1048576.0 : 0, 0, 'f', 0);
    lines << QString("audio %1  %2 ms  underruns %3")
                 .arg(toQ(au.backend))
                 .arg(au.latencyMs, 0, 'f', 1)
                 .arg(au.underruns);
    lines << QString("jobs: %1 running, %2 queued%3   proxies %4")
                 .arg(s_->jobs().running())
                 .arg(s_->jobs().pending())
                 .arg(s_->jobs().paused() ? " (paused)" : "")
                 .arg(s_->proxiesEnabled() ? "on" : "off");
    QFont f("Consolas", 10);
    QFontMetrics fm(f);
    int w = 0;
    for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    const double dpr = video_->devicePixelRatioF();
    QImage img(int((w + 16) * dpr), int((lines.size() * fm.height() + 10) * dpr), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(QColor(0, 0, 0, 170));
    QPainter p(&img);
    p.setFont(f);
    p.setPen(QColor(0x9c, 0xf0, 0x9c));
    for (int i = 0; i < lines.size(); ++i) p.drawText(8, 5 + fm.ascent() + i * fm.height(), lines[i]);
    p.end();
    std::vector<uint8_t> pixels(size_t(img.sizeInBytes()));
    std::memcpy(pixels.data(), img.constBits(), pixels.size());
    e->setHudImage(viewer, img.width(), img.height(), std::move(pixels));
}

} // namespace cut::ui
