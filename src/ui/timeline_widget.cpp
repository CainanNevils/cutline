#include "ui/timeline_widget.h"

#include "audio/mixer.h"
#include "core/edits.h"
#include "core/project_io.h"
#include "engine/media_cache.h"
#include "media/probe.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QHelpEvent>
#include <QLineEdit>
#include <QToolTip>
#include <QUrl>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <cmath>

namespace cut::ui {

namespace {
constexpr int kLabelH = 15;
constexpr double kMinPps = 0.02, kMaxPps = 4000;
const char* kMimeAssets = "application/x-cutline-assets";
const char* kMimeEffect = "application/x-cutline-effect";
const char* kMimeTransition = "application/x-cutline-transition";
const char* kMimePreset = "application/x-cutline-effect-preset";
const char* kMimeSequence = "application/x-cutline-sequence";
const char* kMimeSource = "application/x-cutline-source";

double gainToY(double db, const QRectF& r) {
    // +12 dB at the top of the content area, -60 dB at the bottom
    double f = (std::clamp(db, -60.0, 12.0) + 60.0) / 72.0;
    f = std::pow(f, 1.8);  // spend more room near 0 dB
    return r.bottom() - 3 - f * (r.height() - kLabelH - 6);
}
double yToGain(double y, const QRectF& r) {
    double f = std::clamp((r.bottom() - 3 - y) / (r.height() - kLabelH - 6), 0.0, 1.0);
    return std::pow(f, 1.0 / 1.8) * 72.0 - 60.0;
}
// Horizontal scroll bar that also zooms: drag either end of the thumb to change the visible span
// (like Premiere's timeline zoom bar); double-click zooms to the whole sequence.
class ZoomScrollBar : public QScrollBar {
public:
    explicit ZoomScrollBar(QWidget* parent) : QScrollBar(Qt::Horizontal, parent) { setMouseTracking(true); }
    std::function<void(bool leftEdge, double fraction)> onEdgeDrag;  // fraction of the whole groove
    std::function<void()> onDoubleClick;

protected:
    QRectF thumb() const {
        const double total = double(maximum()) + pageStep();
        const double w = width() - 2.0;
        double x0 = 1 + w * value() / std::max(1.0, total), tw = w * pageStep() / std::max(1.0, total);
        tw = std::max(tw, 26.0);
        x0 = std::min(x0, width() - 1.0 - tw);
        return QRectF(x0, 2, tw, height() - 4);
    }
    int edgeAt(double x) const {
        const QRectF t = thumb();
        if (std::abs(x - t.left()) <= 7) return 1;
        if (std::abs(x - t.right()) <= 7) return 2;
        return 0;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));
        const QRectF t = thumb();
        const bool hot = underMouse();
        p.setPen(Qt::NoPen);
        p.setBrush(hot ? QColor(0x5a, 0x5a, 0x5e) : QColor(0x48, 0x48, 0x4c));
        p.drawRoundedRect(t, t.height() / 2, t.height() / 2);
        p.setBrush(hot ? QColor(0xc8, 0xc8, 0xcc) : QColor(0x8c, 0x8c, 0x90));
        const double r = t.height() / 2 - 1.5;
        p.drawEllipse(QPointF(t.left() + t.height() / 2, t.center().y()), r, r);
        p.drawEllipse(QPointF(t.right() - t.height() / 2, t.center().y()), r, r);
    }
    void mousePressEvent(QMouseEvent* e) override {
        edge_ = e->button() == Qt::LeftButton ? edgeAt(e->position().x()) : 0;
        if (!edge_) QScrollBar::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (edge_ && onEdgeDrag) {
            onEdgeDrag(edge_ == 1, std::clamp((e->position().x() - 1) / std::max(1.0, width() - 2.0), 0.0, 1.0));
            return;
        }
        setCursor(edgeAt(e->position().x()) ? Qt::SizeHorCursor : Qt::ArrowCursor);
        QScrollBar::mouseMoveEvent(e);
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (edge_) {
            edge_ = 0;
            return;
        }
        QScrollBar::mouseReleaseEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        if (onDoubleClick) onDoubleClick();
    }
    void leaveEvent(QEvent* e) override {
        update();
        QScrollBar::leaveEvent(e);
    }

private:
    int edge_ = 0;
};
} // namespace

TimelineWidget::TimelineWidget(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(150);
    auto* zbar = new ZoomScrollBar(this);
    zbar->setToolTip(tr("Drag the ends to zoom, double-click to see the whole sequence"));
    zbar->onEdgeDrag = [this](bool left, double fraction) { zoomEdge(left, fraction); };
    zbar->onDoubleClick = [this] { zoomToFit(); };
    hbar_ = zbar;
    vbar_ = new QScrollBar(Qt::Vertical, this);
    connect(hbar_, &QScrollBar::valueChanged, this, [this](int v) {
        scrollX_ = v;
        update();
    });
    connect(vbar_, &QScrollBar::valueChanged, this, [this](int v) {
        scrollY_ = v;
        update();
    });
    connect(s_, &Session::changed, this, [this] {
        layoutRows();
        updateScrollBars();
        update();
    });
    connect(s_, &Session::structureChanged, this, [this] {
        layoutRows();
        updateScrollBars();
        update();
    });
    connect(s_, &Session::selectionChanged, this, [this] { update(); });
    connect(s_, &Session::projectLoaded, this, &TimelineWidget::zoomToFit);
    connect(s_, &Session::mediaUpdated, this, [this] { update(); });
    connect(s_, &Session::playheadChanged, this, [this](qint64 t) {
        if (s_->playing()) {
            // page-scroll like Premiere when the playhead leaves the view
            double x = timeToX(t);
            if (x > width() - vbar_->width() - 10 || x < bodyLeft()) {
                hbar_->setValue(int(double(t) / kTicksPerSecond * pps_ - 20));
            }
        }
        update();
    });
    layoutRows();
}

// ---------------------------------------------------------------------------------------------
// Geometry

void TimelineWidget::layoutRows() {
    rows_.clear();
    const Sequence& s = s_->seq();
    int y = 0;
    auto add = [&](const Track& t, int index) {
        Row r;
        r.track = t.id;
        r.kind = t.kind;
        r.index = index;
        r.y = y;
        r.h = t.height;
        y += r.h;
        rows_.push_back(r);
    };
    int ci = 0;
    for (const auto& t : s.tracks)
        if (t.kind == TrackKind::Caption) add(t, ci++);
    auto vts = s.tracksOf(TrackKind::Video);
    for (int i = int(vts.size()) - 1; i >= 0; --i) add(*vts[size_t(i)], i);
    y += 6;  // divider between video and audio
    auto ats = s.tracksOf(TrackKind::Audio);
    for (int i = 0; i < int(ats.size()); ++i) add(*ats[size_t(i)], i);
    contentH_ = y + 40;
}

const TimelineWidget::Row* TimelineWidget::rowFor(Id track) const {
    for (const auto& r : rows_)
        if (r.track == track) return &r;
    return nullptr;
}

int TimelineWidget::rowAt(int y) const {
    int cy = y - rulerH_ + scrollY_;
    for (int i = 0; i < int(rows_.size()); ++i)
        if (cy >= rows_[size_t(i)].y && cy < rows_[size_t(i)].y + rows_[size_t(i)].h) return i;
    return -1;
}

double TimelineWidget::timeToX(Tick t) const { return bodyLeft() + double(t) / kTicksPerSecond * pps_ - scrollX_; }
Tick TimelineWidget::xToTime(double x) const {
    return std::max<Tick>(0, Tick((x - bodyLeft() + scrollX_) / pps_ * kTicksPerSecond));
}

QRectF TimelineWidget::clipRect(const Clip& c, const Row& r) const {
    double x0 = timeToX(c.start), x1 = timeToX(c.end());
    double y = rulerH_ + r.y - scrollY_;
    return QRectF(x0, y + 1, std::max(1.0, x1 - x0), r.h - 2);
}

void TimelineWidget::resizeEvent(QResizeEvent*) {
    const int sb = 12;
    hbar_->setGeometry(headerW_, height() - 14, width() - headerW_ - sb, 14);
    vbar_->setGeometry(width() - sb, rulerH_, sb, height() - rulerH_ - sb);
    updateScrollBars();
}

void TimelineWidget::updateScrollBars() {
    const double total = (ticksToSeconds(s_->seq().duration()) + 60) * pps_;
    const int viewW = width() - headerW_ - vbar_->width();
    hbar_->setRange(0, std::max(0, int(total) - viewW));
    hbar_->setPageStep(std::max(1, viewW));
    hbar_->setSingleStep(40);
    const int viewH = height() - rulerH_ - hbar_->height();
    vbar_->setRange(0, std::max(0, contentH_ - viewH));
    vbar_->setPageStep(std::max(1, viewH));
    vbar_->setSingleStep(20);
}

void TimelineWidget::setPixelsPerSecond(double pps, double anchorX) {
    pps = std::clamp(pps, kMinPps, kMaxPps);
    Tick t = xToTime(anchorX);
    pps_ = pps;
    updateScrollBars();
    double newScroll = double(t) / kTicksPerSecond * pps_ - (anchorX - bodyLeft());
    hbar_->setValue(int(std::max(0.0, newScroll)));
    scrollX_ = hbar_->value();
    update();
}

void TimelineWidget::zoomEdge(bool left, double fraction) {
    const double totalSecs = ticksToSeconds(s_->seq().duration()) + 60;
    const double viewW = width() - headerW_ - vbar_->width();
    const double minSpan = viewW / kMaxPps, maxSpan = viewW / kMinPps;
    double t0 = scrollX_ / pps_, t1 = (scrollX_ + viewW) / pps_;
    const double at = fraction * totalSecs;
    if (left) t0 = std::clamp(at, std::max(0.0, t1 - maxSpan), t1 - minSpan);
    else t1 = std::clamp(at, t0 + minSpan, t0 + maxSpan);
    pps_ = std::clamp(viewW / (t1 - t0), kMinPps, kMaxPps);
    updateScrollBars();
    hbar_->setValue(int(t0 * pps_));
    scrollX_ = hbar_->value();
    update();
}

void TimelineWidget::zoomIn() { setPixelsPerSecond(pps_ * 1.5, timeToX(s_->playhead())); }
void TimelineWidget::zoomOut() { setPixelsPerSecond(pps_ / 1.5, timeToX(s_->playhead())); }

void TimelineWidget::zoomToFit() {
    if (width() < headerW_ + 300) {  // not laid out yet: try again once it has a real size
        QTimer::singleShot(100, this, &TimelineWidget::zoomToFit);
        return;
    }
    const double secs = std::max(5.0, ticksToSeconds(s_->seq().duration()));
    const int viewW = width() - headerW_ - vbar_->width() - 20;
    pps_ = std::clamp(viewW / secs, kMinPps, kMaxPps);
    updateScrollBars();
    hbar_->setValue(0);
    scrollX_ = 0;
    update();
}

QPoint TimelineWidget::pointFor(Id track, Tick t) const {
    const Row* r = rowFor(track);
    if (!r) return {};
    // audio rows: near the bottom, away from the volume line in the middle
    const int dy = r->kind == TrackKind::Audio ? r->h - 7 : r->h / 2 + 4;
    return QPoint(int(std::lround(timeToX(t))), rulerH_ + r->y - scrollY_ + dy);
}

void TimelineWidget::ensureVisible(Tick t) {
    double x = timeToX(t);
    if (x < bodyLeft() || x > width() - vbar_->width()) hbar_->setValue(int(double(t) / kTicksPerSecond * pps_ - 100));
}

// ---------------------------------------------------------------------------------------------
// Snapping

std::vector<Tick> TimelineWidget::snapCandidates(const std::vector<Id>& exclude) const {
    const Sequence& s = s_->seq();
    std::vector<Tick> c{0, s_->playhead()};
    for (const auto& cl : s.clips) {
        if (std::find(exclude.begin(), exclude.end(), cl.id) != exclude.end()) continue;
        c.push_back(cl.start);
        c.push_back(cl.end());
    }
    for (const auto& m : s.markers) c.push_back(m.t);
    if (s.inPoint != kInvalidTick) c.push_back(s.inPoint);
    if (s.outPoint != kInvalidTick) c.push_back(s.outPoint);
    for (const auto& cap : s.captions) {
        c.push_back(cap.start);
        c.push_back(cap.end);
    }
    return c;
}

Tick TimelineWidget::snap(Tick t, const std::vector<Id>& exclude, bool* snapped) {
    t = s_->snapToFrame(t);
    snapShown_ = false;
    if (snapped) *snapped = false;
    if (!s_->snapping) return t;
    const Tick thr = Tick(8.0 / pps_ * kTicksPerSecond);
    Tick best = t, bestD = thr + 1;
    for (Tick c : snapCandidates(exclude)) {
        Tick d = std::llabs(c - t);
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    if (bestD <= thr) {
        snapShown_ = true;
        snapAt_ = best;
        if (snapped) *snapped = true;
        return best;
    }
    return t;
}

Tick TimelineWidget::snapDelta(Tick dt, const std::vector<Id>& moving, bool* snapped) {
    dt = s_->snapToFrame(dt);
    snapShown_ = false;
    if (snapped) *snapped = false;
    if (!s_->snapping) return dt;
    const Tick thr = Tick(8.0 / pps_ * kTicksPerSecond);
    auto cands = snapCandidates(moving);
    Tick bestAdj = 0, bestD = thr + 1, at = 0;
    for (Id id : moving) {
        const Clip* c = s_->seq().clip(id);
        if (!c) continue;
        for (Tick edge : {c->start, c->end()}) {
            Tick e = edge + dt;
            for (Tick cand : cands) {
                Tick d = std::llabs(cand - e);
                if (d < bestD) {
                    bestD = d;
                    bestAdj = cand - e;
                    at = cand;
                }
            }
        }
    }
    if (bestD <= thr) {
        snapShown_ = true;
        snapAt_ = at;
        if (snapped) *snapped = true;
        return dt + bestAdj;
    }
    return dt;
}

// ---------------------------------------------------------------------------------------------
// Hit testing

TimelineWidget::Hit TimelineWidget::hitTest(QPointF p) const {
    Hit h;
    if (p.x() < bodyLeft() || p.y() < rulerH_) return h;
    int ri = rowAt(int(p.y()));
    if (ri < 0) return h;
    h.row = ri;
    const Row& r = rows_[size_t(ri)];
    const Sequence& s = s_->seq();
    const double y0 = rulerH_ + r.y - scrollY_;
    if (r.kind == TrackKind::Caption) {
        for (const auto& c : s.captions) {
            double a = timeToX(c.start), b = timeToX(c.end);
            if (p.x() < a - 4 || p.x() > b + 4) continue;
            h.id = c.id;
            if (std::abs(p.x() - a) < 5) h.kind = Hit::CaptionHead;
            else if (std::abs(p.x() - b) < 5) h.kind = Hit::CaptionTail;
            else h.kind = Hit::Caption;
            return h;
        }
        return h;
    }
    // transitions (upper band of the row)
    for (const auto& tr : s.transitions) {
        if (tr.trackId != r.track) continue;
        TickRange rg = transitionRange(s, tr);
        double a = timeToX(rg.a), b = timeToX(rg.b);
        if (p.y() < y0 + kLabelH || p.y() > y0 + r.h * 0.62 || p.x() < a - 3 || p.x() > b + 3) continue;
        h.id = tr.id;
        h.kind = (std::abs(p.x() - a) < 4 || std::abs(p.x() - b) < 4) ? Hit::TransitionEdge : Hit::Transition;
        return h;
    }
    for (const auto& c : s.clips) {
        if (c.trackId != r.track) continue;
        QRectF cr = clipRect(c, r);
        if (p.x() < cr.left() - 1 || p.x() > cr.right() + 1) continue;
        h.id = c.id;
        if (r.kind == TrackKind::Audio) {
            double fi = timeToX(c.start + c.fadeIn), fo = timeToX(c.end() - c.fadeOut);
            if (std::abs(p.y() - (cr.top() + 4)) < 6) {
                if (std::abs(p.x() - std::max(fi, cr.left() + 4)) < 6) {
                    h.kind = Hit::FadeIn;
                    return h;
                }
                if (std::abs(p.x() - std::min(fo, cr.right() - 4)) < 6) {
                    h.kind = Hit::FadeOut;
                    return h;
                }
            }
            if (r.h >= 40) {
                double gy = gainToY(c.volume.p("level", xToTime(p.x()) - c.start), cr);
                if (std::abs(p.y() - gy) < 4 && p.x() > cr.left() + 6 && p.x() < cr.right() - 6) {
                    h.kind = Hit::GainLine;
                    return h;
                }
            }
        }
        const double edge = std::min(7.0, cr.width() / 4);
        if (p.x() - cr.left() < edge) h.kind = Hit::ClipHead;
        else if (cr.right() - p.x() < edge) h.kind = Hit::ClipTail;
        else h.kind = Hit::Clip;
        return h;
    }
    return h;
}

void TimelineWidget::updateCursor(QPointF p) {
    using T = Session::Tool;
    if (p.y() < rulerH_ || p.x() < bodyLeft()) {
        unsetCursor();
        return;
    }
    switch (s_->tool()) {
    case T::Razor: setCursor(Qt::SplitHCursor); return;
    case T::Hand: setCursor(Qt::OpenHandCursor); return;
    case T::Zoom: setCursor(Qt::CrossCursor); return;
    default: break;
    }
    Hit h = hitTest(p);
    switch (h.kind) {
    case Hit::ClipHead:
    case Hit::ClipTail:
    case Hit::CaptionHead:
    case Hit::CaptionTail:
    case Hit::TransitionEdge: setCursor(s_->tool() == T::Ripple ? Qt::SplitHCursor : Qt::SizeHorCursor); break;
    case Hit::FadeIn:
    case Hit::FadeOut: setCursor(Qt::PointingHandCursor); break;
    case Hit::GainLine: setCursor(Qt::SizeVerCursor); break;
    default: unsetCursor();
    }
}

// ---------------------------------------------------------------------------------------------
// Painting

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), col::bg);
    const Sequence& s = s_->seq();
    // track backgrounds
    p.save();
    p.setClipRect(QRect(bodyLeft(), rulerH_, width() - bodyLeft() - vbar_->width(), height() - rulerH_ - hbar_->height()));
    for (const auto& r : rows_) {
        double y = rulerH_ + r.y - scrollY_;
        if (y > height() || y + r.h < rulerH_) continue;
        QColor bg = r.kind == TrackKind::Caption ? QColor(0x22, 0x20, 0x1c) : QColor(0x1f, 0x1f, 0x1f);
        p.fillRect(QRectF(bodyLeft(), y, width(), r.h - 1), bg);
        p.fillRect(QRectF(bodyLeft(), y + r.h - 1, width(), 1), QColor(0x15, 0x15, 0x15));
    }
    // video/audio divider
    for (size_t i = 1; i < rows_.size(); ++i)
        if (rows_[i].kind == TrackKind::Audio && rows_[i - 1].kind != TrackKind::Audio) {
            double y = rulerH_ + rows_[i].y - 6 - scrollY_;
            p.fillRect(QRectF(0, y, width(), 6), QColor(0x2e, 0x2e, 0x2e));
        }
    // in/out range
    if (s.inPoint != kInvalidTick || s.outPoint != kInvalidTick) {
        double a = timeToX(s.inPoint == kInvalidTick ? 0 : s.inPoint);
        double b = timeToX(s.outPoint == kInvalidTick ? s.duration() : s.outPoint);
        p.fillRect(QRectF(a, rulerH_, b - a, height()), QColor(0x2d, 0x8c, 0xeb, 18));
    }
    // clips
    const Tick t0 = xToTime(bodyLeft()), t1 = xToTime(width());
    for (const auto& r : rows_) {
        double y = rulerH_ + r.y - scrollY_;
        if (y > height() || y + r.h < rulerH_) continue;
        if (r.kind == TrackKind::Caption) {
            paintCaptions(p, r);
            continue;
        }
        for (const auto& c : s.clips) {
            if (c.trackId != r.track || c.end() < t0 || c.start > t1) continue;
            paintClip(p, c, r);
        }
        paintTransitions(p, r);
    }
    // empty sequence: say how to get started
    if (s.clips.empty() && s.captions.empty() && mode_ == Mode::None) {
        p.setPen(col::textDim);
        p.setFont(QFont("Segoe UI", 11));
        p.drawText(QRect(bodyLeft(), rulerH_, width() - bodyLeft(), height() - rulerH_ - 20), Qt::AlignCenter,
                   tr("Drag clips here from the Project panel or from Explorer,\nor mark In/Out in the Source monitor and press , (insert) or . (overwrite)"));
    }
    // rubber band
    if (mode_ == Mode::Rubber && !rubber_.isNull()) {
        p.setPen(QPen(col::accent, 1, Qt::DashLine));
        p.setBrush(QColor(0x2d, 0x8c, 0xeb, 40));
        p.drawRect(rubber_.normalized());
    }
    // gap selection
    if (s_->gapTrack && s_->gapTime != kInvalidTick)
        if (const Row* r = rowFor(s_->gapTrack)) {
            Tick a, b;
            if (edit::gapAt(s, s_->gapTrack, s_->gapTime, a, b)) {
                double y = rulerH_ + r->y - scrollY_;
                p.fillRect(QRectF(timeToX(a), y + 2, timeToX(b) - timeToX(a), r->h - 4), QColor(0xf2, 0xc1, 0x4e, 50));
            }
        }
    // snap line
    if (snapShown_ && mode_ != Mode::None) {
        p.setPen(QPen(QColor(255, 255, 255, 200), 1));
        double x = timeToX(snapAt_);
        p.drawLine(QPointF(x, rulerH_), QPointF(x, height()));
    }
    // playhead
    double px = timeToX(s_->playhead());
    p.setPen(QPen(col::playhead, 1));
    p.drawLine(QPointF(px, rulerH_), QPointF(px, height()));
    p.restore();
    paintRuler(p);
    paintHeaders(p);
    // corner
    p.fillRect(QRect(0, 0, headerW_, rulerH_), col::panel);
    p.setPen(col::playhead);
    p.setFont(QFont("Consolas", 11, QFont::Bold));
    p.drawText(QRect(8, 0, headerW_ - 8, rulerH_), Qt::AlignVCenter, toQ(formatTimecode(s_->playhead(), s.settings.fps)));
}

void TimelineWidget::paintRuler(QPainter& p) {
    const Sequence& s = s_->seq();
    const QRectF rr(bodyLeft(), 0, width() - bodyLeft(), rulerH_);
    p.fillRect(rr, col::panel);
    p.save();
    p.setClipRect(rr);
    // choose a label interval >= ~90 px
    const Rational fps = s.settings.fps;
    const double fpsD = fps.toDouble();
    const double units[] = {1 / fpsD, 2 / fpsD, 5 / fpsD, 10 / fpsD, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double step = 3600;
    for (double u : units)
        if (u * pps_ >= 90) {
            step = u;
            break;
        }
    const double minor = step / 5;
    double tStart = std::floor(scrollX_ / pps_ / step) * step;
    double tEnd = (scrollX_ + width()) / pps_;
    p.setFont(QFont("Segoe UI", 8));
    for (double t = tStart; t <= tEnd + step; t += minor) {
        double x = bodyLeft() + t * pps_ - scrollX_;
        bool major = std::abs(std::fmod(t + 1e-9, step)) < minor * 0.5;
        p.setPen(QColor(0x6a, 0x6a, 0x6a));
        p.drawLine(QPointF(x, rulerH_ - (major ? 10 : 4)), QPointF(x, rulerH_));
        if (major) {
            p.setPen(col::textDim);
            p.drawText(QPointF(x + 3, rulerH_ - 12), toQ(formatTimecode(secondsToTicks(t), fps)));
        }
    }
    // in/out bar
    if (s.inPoint != kInvalidTick || s.outPoint != kInvalidTick) {
        double a = timeToX(s.inPoint == kInvalidTick ? 0 : s.inPoint);
        double b = timeToX(s.outPoint == kInvalidTick ? s.duration() : s.outPoint);
        p.fillRect(QRectF(a, rulerH_ - 5, b - a, 4), QColor(0x2d, 0x8c, 0xeb, 160));
    }
    // markers
    for (const auto& m : s.markers) {
        double x = timeToX(m.t);
        QPolygonF poly;
        poly << QPointF(x - 5, 2) << QPointF(x + 5, 2) << QPointF(x + 5, 9) << QPointF(x, 14) << QPointF(x - 5, 9);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromRgba(m.color));
        p.drawPolygon(poly);
        if (!m.name.empty()) {  // on a chip so the ruler's timecodes don't show through
            const QString name = toQ(m.name);
            const QRectF chip(x + 6, 1, p.fontMetrics().horizontalAdvance(name) + 8, 13);
            p.setBrush(col::panel2);
            p.drawRoundedRect(chip, 2, 2);
            p.setPen(col::text);
            p.drawText(chip.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, name);
            p.setPen(Qt::NoPen);
        }
    }
    // playhead head
    double px = timeToX(s_->playhead());
    QPolygonF head;
    head << QPointF(px - 6, rulerH_ - 12) << QPointF(px + 6, rulerH_ - 12) << QPointF(px + 6, rulerH_ - 5)
         << QPointF(px, rulerH_) << QPointF(px - 6, rulerH_ - 5);
    p.setPen(Qt::NoPen);
    p.setBrush(col::playhead);
    p.drawPolygon(head);
    p.restore();
}

void TimelineWidget::paintHeaders(QPainter& p) {
    const Sequence& s = s_->seq();
    p.save();
    p.setClipRect(QRect(0, rulerH_, headerW_, height() - rulerH_));
    p.fillRect(QRect(0, rulerH_, headerW_, height()), col::panel);
    for (const auto& r : rows_) {
        const Track* t = s.track(r.track);
        if (!t) continue;
        double y = rulerH_ + r.y - scrollY_;
        if (y > height() || y + r.h < rulerH_) continue;
        QRectF row(0, y, headerW_ - 1, r.h - 1);
        p.fillRect(row, QColor(0x29, 0x29, 0x29));
        p.fillRect(QRectF(headerW_ - 1, y, 1, r.h), col::border);
        p.fillRect(QRectF(0, y + r.h - 1, headerW_, 1), col::border);
        // target / source patch toggle
        bool targeted = (r.kind == TrackKind::Video && r.index == s_->targetVideo) ||
                        (r.kind == TrackKind::Audio && r.index == s_->targetAudio);
        QRectF tg(4, y + std::min(4.0, r.h / 2.0 - 9), 28, 18);
        p.setPen(Qt::NoPen);
        if (r.kind != TrackKind::Caption) {
            p.setBrush(targeted ? col::accent : QColor(0x3a, 0x3a, 0x3a));
            p.drawRoundedRect(tg, 3, 3);
        }
        p.setPen(col::text);
        p.setFont(QFont("Segoe UI", 8, QFont::Bold));
        p.drawText(tg, Qt::AlignCenter, toQ(t->name));
        // buttons right-aligned
        const double by = y + std::min(4.0, r.h / 2.0 - 9);
        auto button = [&](int slot, const QString& ic, bool on, const QColor& onColor) {
            QRectF b(headerW_ - 24 - slot * 22, by, 18, 18);
            if (on) {
                p.setPen(Qt::NoPen);
                p.setBrush(onColor);
                p.drawRoundedRect(b, 3, 3);
            }
            icon(ic, on ? QColor(Qt::white) : col::textDim).paint(&p, b.toRect().adjusted(2, 2, -2, -2));
        };
        button(0, t->locked ? "lock" : "unlock", t->locked, QColor(0x80, 0x50, 0x30));
        if (r.kind == TrackKind::Video || r.kind == TrackKind::Caption)
            button(1, t->visible ? "eye" : "eyeoff", !t->visible, QColor(0x60, 0x30, 0x30));
        if (r.kind == TrackKind::Audio) {
            button(1, t->muted ? "mute" : "speaker", t->muted, QColor(0x2f, 0x7a, 0x47));
            QRectF b(headerW_ - 24 - 2 * 22, by, 18, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(t->solo ? QColor(0xc8, 0xa0, 0x20) : QColor(0x3a, 0x3a, 0x3a));
            p.drawRoundedRect(b, 3, 3);
            p.setPen(t->solo ? Qt::black : col::textDim);
            p.drawText(b, Qt::AlignCenter, "S");
        }
        if (t->locked) p.fillRect(QRectF(bodyLeft(), y, 4, r.h), QColor(0x80, 0x50, 0x30));
    }
    p.restore();
}

void TimelineWidget::paintTransitions(QPainter& p, const Row& r) {
    const Sequence& s = s_->seq();
    for (const auto& tr : s.transitions) {
        if (tr.trackId != r.track) continue;
        TickRange rg = transitionRange(s, tr);
        double a = timeToX(rg.a), b = timeToX(rg.b);
        if (b < bodyLeft() || a > width()) continue;
        double y = rulerH_ + r.y - scrollY_ + kLabelH;
        QRectF box(a, y, std::max(4.0, b - a), (r.h - 2) * 0.62 - kLabelH);
        QLinearGradient g(box.topLeft(), box.topRight());
        g.setColorAt(0, QColor(0x9a, 0x7a, 0xd8, 200));
        g.setColorAt(1, QColor(0x6a, 0x4a, 0xa8, 200));
        p.setPen(tr.id == s_->selectedTransition() ? QPen(col::selection, 2) : QPen(QColor(0x30, 0x20, 0x50), 1));
        p.setBrush(g);
        p.drawRect(box);
        p.setPen(QColor(255, 255, 255, 90));
        p.drawLine(box.bottomLeft(), box.topRight());
        if (box.width() > 60) {
            p.setPen(Qt::white);
            p.setFont(QFont("Segoe UI", 7));
            p.drawText(box.adjusted(3, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, transitionName(tr.type));
        }
        if (edit::transitionMissingHandles(s, s_->project().assets, tr)) {
            p.setBrush(QColor(0xe0, 0x50, 0x40));
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(box.right() - 6, box.top() + 6), 4, 4);
        }
    }
}

void TimelineWidget::paintCaptions(QPainter& p, const Row& r) {
    const Sequence& s = s_->seq();
    double y = rulerH_ + r.y - scrollY_;
    p.setFont(QFont("Segoe UI", 8));
    for (const auto& c : s.captions) {
        double a = timeToX(c.start), b = timeToX(c.end);
        if (b < bodyLeft() || a > width()) continue;
        QRectF box(a, y + 2, std::max(2.0, b - a - 1), r.h - 4);
        bool sel = std::find(s_->selectedCaptions.begin(), s_->selectedCaptions.end(), c.id) != s_->selectedCaptions.end();
        p.setPen(sel ? QPen(col::selection, 2) : QPen(QColor(0x60, 0x48, 0x10)));
        p.setBrush(QColor(0xb0, 0x82, 0x2c));
        p.drawRoundedRect(box, 2, 2);
        p.setPen(Qt::black);
        p.drawText(box.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(toQ(c.text).replace('\n', ' '), Qt::ElideRight, int(box.width()) - 6));
    }
}

void TimelineWidget::paintClip(QPainter& p, const Clip& c, const Row& r) {
    QRectF cr = clipRect(c, r);
    const Sequence& s = s_->seq();
    const Asset* a = c.kind == ClipKind::Media ? s_->project().asset(c.assetId) : nullptr;
    const bool offline = (c.kind == ClipKind::Media && (!a || a->offline)) ||
                         (c.kind == ClipKind::Nested && s_->tabOfSequence(c.nestedSeq) < 0);
    QColor base = c.kind == ClipKind::Text ? col::title : r.kind == TrackKind::Audio ? col::audio : col::video;
    if (c.kind == ClipKind::Nested) base = QColor(0x3c, 0xb0, 0xa0);  // nests: teal
    if (c.kind == ClipKind::Adjustment) base = QColor(0x9a, 0x8c, 0xc0);
    if (c.kind == ClipKind::Matte) base = QColor::fromRgb(c.matteColor).lighter(c.matteColor & 0xC0C0C0 ? 100 : 260);
    if (c.label) base = QColor::fromRgba(c.label);
    if (offline) base = col::offline;
    QColor body = base.darker(170);
    if (!c.enabled) body = QColor(0x3a, 0x3a, 0x3a);
    const bool sel = s_->isSelected(c.id);
    p.setPen(Qt::NoPen);
    p.setBrush(body);
    p.drawRect(cr);
    // label bar
    QRectF lab(cr.left(), cr.top(), cr.width(), std::min<double>(kLabelH, cr.height()));
    p.fillRect(lab, c.enabled ? base.darker(115) : QColor(0x50, 0x50, 0x50));
    QRectF content(cr.left(), cr.top() + kLabelH, cr.width(), cr.height() - kLabelH);
    const double vx0 = std::max<double>(cr.left(), bodyLeft()), vx1 = std::min<double>(cr.right(), width());

    // thumbnails
    if (r.kind == TrackKind::Video && a && content.height() > 14 && !offline) {
        if (ThumbStripPtr ts = s_->thumbs(a->id)) {
            const double th = content.height() - 2;
            const double tw = th * ts->w / std::max(1, ts->h);
            p.save();
            p.setClipRect(content.intersected(QRectF(vx0, content.top(), vx1 - vx0, content.height())));
            double x = cr.left();
            if (x < vx0 - tw) x += std::floor((vx0 - x) / tw) * tw;
            for (; x < vx1; x += tw) {
                Tick t = xToTime(x);
                int idx = ts->indexAt(c.isHold() ? c.sourceIn : c.sourceAt(t, s.frameDur()));
                if (idx < 0) continue;
                p.drawImage(QRectF(x, content.top() + 1, tw, th), ts->strip, QRectF(idx * ts->w, 0, ts->w, ts->h));
            }
            p.fillRect(content, QColor(0, 0, 0, c.enabled ? 30 : 140));
            p.restore();
        }
    }
    // waveform
    if (r.kind == TrackKind::Audio && a && content.height() > 8 && !offline) {
        if (WaveformPtr w = s_->waveform(a->id, c.stream)) {
            p.save();
            p.setClipRect(content);
            const double cy = content.center().y(), half = content.height() / 2 - 1;
            QVector<QLineF> lines;
            lines.reserve(int(vx1 - vx0) + 2);
            for (double x = std::floor(vx0); x < vx1; x += 1) {
                Tick ta = xToTime(x), tb = xToTime(x + 1);
                if (ta >= c.end() || tb <= c.start) continue;
                Tick sa = c.sourceAt(ta, 1), sb = c.sourceAt(std::min(tb, c.end() - 1), 1);
                if (sa > sb) std::swap(sa, sb);
                float mn, mx;
                if (c.isHold() || !w->range(ticksToSamples(sa, 48000), ticksToSamples(sb, 48000) + 1, mn, mx)) continue;
                double g = clipGainAt(c, ta - c.start);
                lines.push_back(QLineF(x, cy - std::min(1.0, mx * g) * half, x, cy - std::max(-1.0, mn * g) * half));
            }
            p.setPen(QColor(0x9f, 0xe6, 0xb2, c.enabled ? 220 : 90));
            p.drawLines(lines);
            p.restore();
        }
    }
    // text clip preview
    if (c.kind == ClipKind::Text && content.height() > 10) {
        p.setPen(QColor(255, 255, 255, 170));
        p.setFont(QFont("Segoe UI", 8, QFont::Bold));
        p.drawText(content.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, toQ(c.text).replace('\n', ' '));
    }
    if (offline) {
        p.save();
        p.setClipRect(cr);
        p.setPen(QPen(QColor(255, 255, 255, 40), 6));
        for (double x = cr.left() - cr.height(); x < cr.right(); x += 16)
            p.drawLine(QPointF(x, cr.bottom()), QPointF(x + cr.height(), cr.top()));
        p.restore();
    }
    // audio fades + gain line
    if (r.kind == TrackKind::Audio) {
        p.save();
        p.setClipRect(cr);
        p.setPen(QPen(QColor(255, 255, 255, 150), 1));
        if (c.fadeIn > 0) p.drawLine(QPointF(cr.left(), cr.bottom()), QPointF(timeToX(c.start + c.fadeIn), cr.top() + kLabelH));
        if (c.fadeOut > 0) p.drawLine(QPointF(timeToX(c.end() - c.fadeOut), cr.top() + kLabelH), QPointF(cr.right(), cr.bottom()));
        if (r.h >= 40) {
            p.setPen(QPen(QColor(0xf0, 0xe0, 0x60, 200), 1));
            const Param* lv = c.volume.params.count("level") ? &c.volume.params.at("level") : nullptr;
            if (!lv || lv->keys.empty()) {
                double gy = gainToY(c.volume.p("level", 0), cr);
                p.drawLine(QPointF(cr.left(), gy), QPointF(cr.right(), gy));
            } else {
                QPolygonF poly;
                for (double x = vx0; x <= vx1; x += 3) poly << QPointF(x, gainToY(lv->at(xToTime(x) - c.start), cr));
                p.drawPolyline(poly);
                p.setBrush(QColor(0xf0, 0xe0, 0x60));
                for (const auto& k : lv->keys) {
                    QPointF kp(timeToX(c.start + k.t), gainToY(k.v, cr));
                    QPolygonF d;
                    d << kp + QPointF(0, -4) << kp + QPointF(4, 0) << kp + QPointF(0, 4) << kp + QPointF(-4, 0);
                    p.drawPolygon(d);
                }
            }
        }
        // fade handles
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, sel ? 230 : 120));
        p.drawRect(QRectF(std::max(timeToX(c.start + c.fadeIn), cr.left() + 4) - 3, cr.top() + 1, 6, 6));
        p.drawRect(QRectF(std::min(timeToX(c.end() - c.fadeOut), cr.right() - 4) - 3, cr.top() + 1, 6, 6));
        p.restore();
    }
    // label text + badges
    if (lab.width() > 12) {
        QString name = toQ(c.name);
        QStringList badges;
        bool fx = !c.effects.empty();
        for (const auto& [k, prm] : c.motion.params) fx |= prm.animated();
        for (const auto& [k, prm] : c.opacity.params) fx |= prm.animated();
        if (fx) badges << "fx";
        if (c.isHold()) badges << tr("hold");
        else if (c.speed != 1.0 || c.reverse) badges << QString("%1%2%").arg(c.reverse ? "-" : "").arg(c.speed * 100, 0, 'g', 4);
        if (offline) badges << tr("offline");
        p.setFont(QFont("Segoe UI", 8));
        p.setPen(c.enabled ? QColor(0xf0, 0xf0, 0xf0) : col::textDim);
        QString label = badges.isEmpty() ? name : badges.join(" ") + "  " + name;
        QRectF tr = lab.adjusted(4, 0, -4, 0);
        tr.setLeft(std::max(tr.left(), double(bodyLeft()) + 3));
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(label, Qt::ElideRight, int(tr.width())));
    }
    // border
    p.setBrush(Qt::NoBrush);
    if (sel) {
        p.setPen(QPen(col::selection, 2));
        p.drawRect(cr.adjusted(1, 1, -1, -1));
    } else {
        p.setPen(QPen(QColor(0, 0, 0, 120), 1));
        p.drawRect(cr);
    }
    if (c.linkId && cr.width() > 30 && !sel) {
        // tiny link mark
        p.setPen(QColor(255, 255, 255, 60));
        p.drawLine(QPointF(cr.right() - 6, cr.bottom() - 3), QPointF(cr.right() - 2, cr.bottom() - 3));
    }
}

// ---------------------------------------------------------------------------------------------
// Mouse

void TimelineWidget::headerClick(const Row& r, QPointF pos, Qt::MouseButton b) {
    if (b != Qt::LeftButton) return;
    const Track* t = s_->seq().track(r.track);
    if (!t) return;
    const double y = rulerH_ + r.y - scrollY_;
    const double by = y + std::min(4.0, r.h / 2.0 - 9);
    auto in = [&](double x0) { return pos.x() >= x0 && pos.x() < x0 + 18 && pos.y() >= by && pos.y() < by + 18; };
    const Id id = r.track;
    if (in(headerW_ - 24)) {
        s_->edit(t->locked ? tr("Unlock Track") : tr("Lock Track"), [&](Project& p) {
            Track* x = p.sequence.track(id);
            x->locked = !x->locked;
        });
    } else if (in(headerW_ - 46)) {
        if (r.kind == TrackKind::Audio)
            s_->edit(tr("Mute Track"), [&](Project& p) {
                Track* x = p.sequence.track(id);
                x->muted = !x->muted;
            });
        else
            s_->edit(tr("Toggle Track Output"), [&](Project& p) {
                Track* x = p.sequence.track(id);
                x->visible = !x->visible;
            });
    } else if (r.kind == TrackKind::Audio && in(headerW_ - 68)) {
        s_->edit(tr("Solo Track"), [&](Project& p) {
            Track* x = p.sequence.track(id);
            x->solo = !x->solo;
        });
    } else if (pos.x() < 34) {
        if (r.kind == TrackKind::Video) s_->targetVideo = r.index;
        if (r.kind == TrackKind::Audio) s_->targetAudio = r.index;
        update();
    }
}

int TimelineWidget::trackDeltaFor(const Row& from, int y) const {
    int ri = rowAt(y);
    if (ri < 0) {
        // above the first row -> one past the top video track; below -> one past the last audio track
        int cy = y - rulerH_ + scrollY_;
        if (from.kind == TrackKind::Video && cy < (rows_.empty() ? 0 : rows_.front().y))
            return int(s_->seq().tracksOf(TrackKind::Video).size()) - from.index;
        if (from.kind == TrackKind::Audio && cy >= contentH_ - 40)
            return int(s_->seq().tracksOf(TrackKind::Audio).size()) - from.index;
        return 0;
    }
    const Row& to = rows_[size_t(ri)];
    if (to.kind != from.kind) return 0;
    return to.index - from.index;
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    setFocus();
    const QPointF pos = e->position();
    press_ = pos;
    pressTime_ = xToTime(pos.x());
    snapShown_ = false;
    if (e->button() == Qt::MiddleButton) {
        mode_ = Mode::Pan;
        panStart_ = scrollX_;
        return;
    }
    if (pos.y() < rulerH_ && pos.x() >= bodyLeft()) {
        if (e->button() == Qt::LeftButton) {
            s_->stop();
            mode_ = Mode::Scrub;
            Tick t = (e->modifiers() & Qt::ShiftModifier) ? snap(pressTime_, {}) : pressTime_;
            s_->scrubPlayhead(t);
        }
        return;
    }
    if (pos.y() < rulerH_ && e->button() == Qt::LeftButton) {  // the timecode in the corner
        editTimecode();
        return;
    }
    if (pos.x() < bodyLeft()) {
        int ri = rowAt(int(pos.y()));
        if (ri >= 0) headerClick(rows_[size_t(ri)], pos, e->button());
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    using T = Session::Tool;
    const T tool = s_->tool();
    if (tool == T::Hand) {
        mode_ = Mode::Pan;
        panStart_ = scrollX_;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (tool == T::Zoom) {
        setPixelsPerSecond((e->modifiers() & Qt::AltModifier) ? pps_ / 2 : pps_ * 2, pos.x());
        return;
    }
    Hit h = hitTest(pos);
    pressHit_ = h;
    const Sequence& s = s_->seq();
    if (tool == T::Razor) {
        if (h.kind == Hit::Clip || h.kind == Hit::ClipHead || h.kind == Hit::ClipTail) {
            Tick t = snap(pressTime_, {});
            Id id = h.id;
            bool all = e->modifiers() & Qt::ShiftModifier;
            bool alone = e->modifiers() & Qt::AltModifier;
            s_->edit(tr("Razor"), [&](Project& p) {
                if (all) edit::addEdit(p.sequence, t, {});
                else edit::razor(p.sequence, id, t, alone);
            });
        }
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier, ctrl = e->modifiers() & Qt::ControlModifier,
               alt = e->modifiers() & Qt::AltModifier;
    s_->gapTrack = 0;
    switch (h.kind) {
    case Hit::Caption:
    case Hit::CaptionHead:
    case Hit::CaptionTail: {
        s_->setSelection({});
        if (shift || ctrl) {
            auto& sc = s_->selectedCaptions;
            auto it = std::find(sc.begin(), sc.end(), h.id);
            if (it == sc.end()) sc.push_back(h.id);
            else sc.erase(it);
        } else if (std::find(s_->selectedCaptions.begin(), s_->selectedCaptions.end(), h.id) == s_->selectedCaptions.end())
            s_->selectedCaptions = {h.id};
        emit s_->selectionChanged();
        mode_ = h.kind == Hit::Caption ? Mode::CaptionMove : h.kind == Hit::CaptionHead ? Mode::CaptionHead : Mode::CaptionTail;
        for (const auto& c : s.captions)
            if (c.id == h.id) origEdge_ = h.kind == Hit::CaptionTail ? c.end : c.start;
        s_->beginDrag(tr("Edit Caption Timing"));
        update();
        return;
    }
    case Hit::Transition:
    case Hit::TransitionEdge:
        s_->setSelectedTransition(h.id);
        if (h.kind == Hit::TransitionEdge) {
            mode_ = Mode::TransitionResize;
            s_->beginDrag(tr("Transition Duration"));
        }
        return;
    case Hit::FadeIn:
    case Hit::FadeOut:
        if (!s_->isSelected(h.id)) s_->setSelection({h.id}, false);
        mode_ = h.kind == Hit::FadeIn ? Mode::FadeIn : Mode::FadeOut;
        if (const Clip* c = s.clip(h.id)) origEdge_ = h.kind == Hit::FadeIn ? c->fadeIn : c->fadeOut;
        s_->beginDrag(tr("Audio Fade"));
        return;
    case Hit::GainLine:
        if (!s_->isSelected(h.id)) s_->setSelection({h.id}, false);
        mode_ = Mode::Gain;
        if (const Clip* c = s.clip(h.id)) origValue_ = c->volume.p("level", pressTime_ - c->start);
        if (ctrl) {
            // Ctrl+click on the line adds a volume keyframe
            Id id = h.id;
            Tick local = pressTime_;
            s_->edit(tr("Add Volume Keyframe"), [&](Project& p) {
                if (Clip* c = p.sequence.clip(id)) {
                    Param& lv = c->volume.params["level"];
                    lv.addKey(s_->snapToFrame(local - c->start), lv.at(local - c->start));
                }
            });
            mode_ = Mode::None;
            return;
        }
        s_->beginDrag(tr("Clip Volume"));
        return;
    case Hit::ClipHead:
    case Hit::ClipTail: {
        const Clip* c = s.clip(h.id);
        const Track* t = c ? s.track(c->trackId) : nullptr;
        if (!c || (t && t->locked)) return;
        if (!s_->isSelected(h.id)) s_->setSelection({h.id}, !alt);
        mode_ = h.kind == Hit::ClipHead ? Mode::TrimHead : Mode::TrimTail;
        origEdge_ = h.kind == Hit::ClipHead ? c->start : c->end();
        s_->beginDrag(tool == T::RateStretch ? tr("Rate Stretch") : tool == T::Ripple || ctrl ? tr("Ripple Trim") : tr("Trim"));
        return;
    }
    case Hit::Clip: {
        const Clip* c = s.clip(h.id);
        const Track* t = c ? s.track(c->trackId) : nullptr;
        if (!c || (t && t->locked)) return;
        if (shift || ctrl) {
            std::vector<Id> sel = s_->selection();
            auto group = alt ? std::vector<Id>{h.id} : s.linkedClips(h.id);
            bool has = s_->isSelected(h.id);
            for (Id g : group) {
                auto it = std::find(sel.begin(), sel.end(), g);
                if (has && it != sel.end()) sel.erase(it);
                else if (!has && it == sel.end()) sel.push_back(g);
            }
            s_->setSelection(sel, false);
        } else if (!s_->isSelected(h.id)) {
            s_->setSelection({h.id}, !alt);
        }
        s_->selectedCaptions.clear();
        mode_ = Mode::Pending;
        return;
    }
    default: {
        if (!shift) {
            s_->setSelection({});
            s_->selectedCaptions.clear();
            if (s_->selectedTransition()) s_->setSelectedTransition(0);
        }
        if (h.row >= 0) {
            Tick a, b;
            if (edit::gapAt(s, rows_[size_t(h.row)].track, pressTime_, a, b)) {
                s_->gapTrack = rows_[size_t(h.row)].track;
                s_->gapTime = pressTime_;
            }
        }
        mode_ = Mode::Rubber;
        rubber_ = QRectF(pos, pos);
        update();
    }
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (mode_ == Mode::None) {
        updateCursor(pos);
        return;
    }
    const Sequence& s = s_->seq();
    const Tick t = xToTime(pos.x());
    switch (mode_) {
    case Mode::Scrub: {
        Tick st = (e->modifiers() & Qt::ShiftModifier) ? snap(t, {}) : t;
        s_->scrubPlayhead(st);
        if (pos.x() > width() - 30) hbar_->setValue(hbar_->value() + 10);
        if (pos.x() < bodyLeft() + 10) hbar_->setValue(hbar_->value() - 10);
        break;
    }
    case Mode::Pan: hbar_->setValue(int(panStart_ - (pos.x() - press_.x()))); break;
    case Mode::Pending:
        if (std::abs(pos.x() - press_.x()) + std::abs(pos.y() - press_.y()) < 5) break;
        moving_ = s_->selection();
        {
            std::vector<Id> unlocked;
            for (Id id : moving_)
                if (const Clip* c = s.clip(id)) {
                    const Track* tr = s.track(c->trackId);
                    if (tr && !tr->locked) unlocked.push_back(id);
                }
            moving_ = unlocked;
        }
        if (moving_.empty()) {
            mode_ = Mode::None;
            break;
        }
        s_->beginDrag((e->modifiers() & Qt::AltModifier) ? tr("Duplicate") : tr("Move"));
        mode_ = Mode::Move;
        dropInsert_ = e->modifiers() & Qt::AltModifier;  // reused as "duplicate" flag for this drag
        [[fallthrough]];
    case Mode::Move: {
        bool snapped = false;
        Tick dt = snapDelta(t - pressTime_, moving_, &snapped);
        const Row* pr = pressHit_.row >= 0 ? &rows_[size_t(pressHit_.row)] : nullptr;
        int dTrack = pr ? trackDeltaFor(*pr, int(pos.y())) : 0;
        int dV = pr && pr->kind == TrackKind::Video ? dTrack : 0;
        int dA = pr && pr->kind == TrackKind::Audio ? dTrack : 0;
        const bool insert = e->modifiers() & Qt::ControlModifier;
        const bool duplicate = dropInsert_;
        auto ids = moving_;
        s_->updateDrag([&](Project& p) {
            Sequence& sq = p.sequence;
            // create tracks when dragging past the last one
            int maxV = 0, maxA = 0;
            for (Id id : ids)
                if (const Clip* c = sq.clip(id)) {
                    const Track* tr = sq.track(c->trackId);
                    int idx = sq.trackIndexInKind(c->trackId);
                    if (tr->kind == TrackKind::Video) maxV = std::max(maxV, idx + dV + 1);
                    if (tr->kind == TrackKind::Audio) maxA = std::max(maxA, idx + dA + 1);
                }
            if (dV > 0) edit::ensureTracks(sq, TrackKind::Video, maxV);
            if (dA > 0) edit::ensureTracks(sq, TrackKind::Audio, maxA);
            if (duplicate) edit::duplicateClips(sq, ids, dt, dV, dA);
            else edit::moveClips(sq, ids, dt, dV, dA, insert);
        });
        break;
    }
    case Mode::TrimHead:
    case Mode::TrimTail: {
        Id id = pressHit_.id;
        Tick nt = snap(t, s.linkedClips(id));
        const bool ripple = s_->tool() == Session::Tool::Ripple || (e->modifiers() & Qt::ControlModifier);
        const bool alone = e->modifiers() & Qt::AltModifier;
        const auto edge = mode_ == Mode::TrimHead ? edit::Edge::Head : edit::Edge::Tail;
        if (s_->tool() == Session::Tool::RateStretch) {
            // rate stretch: the tail sets a new length and the speed follows (same media span)
            if (edge == edit::Edge::Tail)
                s_->updateDrag([&](Project& p) {
                    Clip* c = p.sequence.clip(id);
                    if (!c || c->kind != ClipKind::Media || c->isHold()) return;
                    const Tick len = std::max(p.sequence.frameDur(), nt - c->start);
                    const double speed = std::clamp(double(c->sourceSpan()) / double(len), 0.1, 10.0);
                    edit::setSpeed(p.sequence, p.assets, id, speed, c->reverse, false, speed < 1.0);
                });
            break;
        }
        s_->updateDrag([&](Project& p) { edit::trimClip(p.sequence, p.assets, id, edge, nt, ripple, alone); });
        if (const Clip* tc = s_->seq().clip(id); tc && s_->engine() && !s_->playing())
            s_->engine()->setPreviewOverride(s_->programViewer(),
                                             mode_ == Mode::TrimHead ? tc->start : std::max(tc->start, tc->end() - s_->seq().frameDur()));
        break;
    }
    case Mode::FadeIn:
    case Mode::FadeOut: {
        Id id = pressHit_.id;
        const bool in = mode_ == Mode::FadeIn;
        s_->updateDrag([&](Project& p) {
            if (Clip* c = p.sequence.clip(id)) {
                // relative to where the handle was grabbed: no jump on the first move
                Tick v = in ? origEdge_ + (t - pressTime_) : origEdge_ - (t - pressTime_);
                v = std::clamp<Tick>(s_->snapToFrame(v), 0, c->duration);
                (in ? c->fadeIn : c->fadeOut) = v;
            }
        });
        break;
    }
    case Mode::Gain: {
        Id id = pressHit_.id;
        const double perPx = (e->modifiers() & Qt::ShiftModifier) ? 0.05 : 0.25;
        const double delta = -(pos.y() - press_.y()) * perPx;
        s_->updateDrag([&](Project& p) {
            if (Clip* c = p.sequence.clip(id)) {
                Param& lv = c->volume.params["level"];
                if (lv.keys.empty()) lv.value = std::clamp(origValue_ + delta, -96.0, 12.0);
                else
                    for (auto& k : lv.keys) k.v = std::clamp(k.v + delta, -96.0, 12.0);  // keys are the drag-start state
            }
        });
        break;
    }
    case Mode::TransitionResize: {
        Id id = s_->selectedTransition();
        s_->updateDrag([&](Project& p) {
            Transition* tr = p.sequence.transition(id);
            if (!tr) return;
            const Clip* b = tr->clipB ? p.sequence.clip(tr->clipB) : nullptr;
            const Clip* a = tr->clipA ? p.sequence.clip(tr->clipA) : nullptr;
            Tick cut = b ? b->start : a ? a->end() : 0;
            Tick d = std::llabs(t - cut);
            if (tr->align == TransitionAlign::Center) d *= 2;
            tr->duration = std::max(p.sequence.frameDur(), s_->snapToFrame(d));
        });
        break;
    }
    case Mode::CaptionMove:
    case Mode::CaptionHead:
    case Mode::CaptionTail: {
        Tick dt = s_->snapToFrame(t - pressTime_);
        Id id = pressHit_.id;
        const Mode m = mode_;
        Tick edgeT = snap(origEdge_ + dt, {});
        auto sel = s_->selectedCaptions;
        s_->updateDrag([&](Project& p) {
            const Tick fd = p.sequence.frameDur();
            for (auto& c : p.sequence.captions) {
                if (m == Mode::CaptionMove) {
                    if (std::find(sel.begin(), sel.end(), c.id) == sel.end()) continue;
                    Tick d = std::max(dt, -c.start);
                    c.start += d;
                    c.end += d;
                } else if (c.id == id) {
                    if (m == Mode::CaptionHead) c.start = std::clamp<Tick>(edgeT, 0, c.end - fd);
                    else c.end = std::max(edgeT, c.start + fd);
                }
            }
        });
        break;
    }
    case Mode::Rubber: {
        rubber_.setBottomRight(pos);
        QRectF rn = rubber_.normalized();
        if (rn.width() < 3 && rn.height() < 3) break;
        s_->gapTrack = 0;
        std::vector<Id> sel;
        for (const auto& r : rows_) {
            if (r.kind == TrackKind::Caption) continue;
            for (const auto& c : s.clips)
                if (c.trackId == r.track && clipRect(c, r).intersects(rn)) sel.push_back(c.id);
        }
        s_->setSelection(sel, !(e->modifiers() & Qt::AltModifier));
        update();
        break;
    }
    default: break;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    if ((mode_ == Mode::TrimHead || mode_ == Mode::TrimTail) && s_->engine())
        s_->engine()->setPreviewOverride(s_->programViewer(), kInvalidTick);
    switch (mode_) {
    case Mode::Pending:
        // plain click on an already-selected clip: select just it (and its links)
        if (!(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) && pressHit_.id)
            s_->setSelection({pressHit_.id}, !(e->modifiers() & Qt::AltModifier));
        break;
    case Mode::Move:
    case Mode::TrimHead:
    case Mode::TrimTail:
    case Mode::FadeIn:
    case Mode::FadeOut:
    case Mode::Gain:
    case Mode::TransitionResize:
    case Mode::CaptionMove:
    case Mode::CaptionHead:
    case Mode::CaptionTail: s_->endDrag(true); break;
    default: break;
    }
    if (mode_ == Mode::Pan) unsetCursor();
    mode_ = Mode::None;
    snapShown_ = false;
    rubber_ = QRectF();
    dropInsert_ = false;
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (pos.y() < rulerH_) {
        // rename a marker
        for (const auto& m : s_->seq().markers)
            if (std::abs(timeToX(m.t) - pos.x()) < 6) {
                bool ok = false;
                QString n = QInputDialog::getText(this, tr("Marker"), tr("Name:"), QLineEdit::Normal, toQ(m.name), &ok);
                Id id = m.id;
                if (ok) s_->edit(tr("Rename Marker"), [&](Project& p) {
                        for (auto& x : p.sequence.markers)
                            if (x.id == id) x.name = toS(n);
                    });
                return;
            }
        return;
    }
    Hit h = hitTest(pos);
    if (h.kind == Hit::Caption) {
        for (const auto& c : s_->seq().captions)
            if (c.id == h.id) {
                bool ok = false;
                QString t = QInputDialog::getMultiLineText(this, tr("Caption"), tr("Text:"), toQ(c.text), &ok);
                Id id = c.id;
                if (ok) s_->edit(tr("Edit Caption"), [&](Project& p) {
                        for (auto& x : p.sequence.captions)
                            if (x.id == id) x.text = toS(t);
                    });
            }
        return;
    }
    if (h.kind == Hit::Clip) {
        const Clip* c = s_->seq().clip(h.id);
        if (!c) return;
        if (c->kind == ClipKind::Nested) {
            s_->openNested(c->id);
            return;
        }
        if (c->kind == ClipKind::Text) {
            bool ok = false;
            QString t = QInputDialog::getMultiLineText(this, tr("Edit Title"), tr("Text:"), toQ(c->text), &ok);
            Id id = c->id;
            if (ok) s_->edit(tr("Edit Title Text"), [&](Project& p) {
                    if (Clip* x = p.sequence.clip(id)) x->text = toS(t);
                });
        } else {
            s_->openInSource(c->assetId);
        }
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    const QPoint d = e->angleDelta();
    if (e->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) {
        int dy = d.y() != 0 ? d.y() : d.x();
        setPixelsPerSecond(pps_ * (dy > 0 ? 1.25 : 0.8), e->position().x());
    } else if ((e->modifiers() & Qt::ShiftModifier) || std::abs(d.x()) > std::abs(d.y())) {
        int v = d.x() != 0 ? d.x() : d.y();
        hbar_->setValue(hbar_->value() - v);
    } else {
        vbar_->setValue(vbar_->value() - d.y() / 2);
    }
    e->accept();
}

void TimelineWidget::editTimecode() {
    auto* ed = new QLineEdit(this);
    ed->setObjectName("timelineTimecode");
    ed->setGeometry(4, 2, headerW_ - 8, rulerH_ - 4);
    ed->setText(toQ(formatTimecode(s_->playhead(), s_->seq().settings.fps)));
    ed->setStyleSheet("QLineEdit { color: #4d9dff; font-weight: 600; }");
    ed->selectAll();
    ed->show();
    ed->setFocus();
    connect(ed, &QLineEdit::returnPressed, this, [this, ed] {
        Tick t;
        const QString text = ed->text().trimmed();
        if (parseTimecode(toS(text), s_->seq().settings.fps, t)) {
            if (text.startsWith('+') || text.startsWith('-')) t = std::max<Tick>(0, s_->playhead() + t);
            s_->setPlayhead(s_->snapToFrame(t));
        }
        ed->clearFocus();
    });
    connect(ed, &QLineEdit::editingFinished, ed, [this, ed] {
        ed->deleteLater();
        setFocus();
    });
}

bool TimelineWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        if (he->pos().y() < rulerH_ && he->pos().x() < bodyLeft()) {
            QToolTip::showText(he->globalPos(), tr("Playhead position - click to type a timecode (+/- for relative)"), this);
            return true;
        }
        Hit h = hitTest(he->pos());
        const Sequence& s = s_->seq();
        const Rational fps = s.settings.fps;
        QString tip;
        if (h.kind == Hit::Clip || h.kind == Hit::ClipHead || h.kind == Hit::ClipTail) {
            if (const Clip* c = s.clip(h.id)) {
                tip = QString("<b>%1</b><br>Start %2 &nbsp; Duration %3").arg(toQ(c->name).toHtmlEscaped(),
                          toQ(formatTimecode(c->start, fps)), toQ(formatTimecode(c->duration, fps)));
                if (c->kind == ClipKind::Media) {
                    tip += QString("<br>Source In %1").arg(toQ(formatTimecode(c->isHold() ? c->sourceIn : (c->reverse ? c->sourceOut() : c->sourceIn), fps)));
                    if (c->isHold()) tip += tr(" (frame hold)");
                    else if (c->speed != 1.0 || c->reverse) tip += QString("<br>Speed %1%2%").arg(c->reverse ? "-" : "").arg(c->speed * 100, 0, 'g', 4);
                    if (c->gainDb != 0) tip += QString("<br>Gain %1 dB").arg(c->gainDb, 0, 'f', 1);
                }
                if (!c->effects.empty()) {
                    QStringList fx;
                    for (const auto& ef : c->effects)
                        if (const EffectDef* d = findEffectDef(ef.type)) fx << toQ(d->label) + (ef.enabled ? "" : tr(" (off)"));
                    tip += "<br>" + tr("Effects: %1").arg(fx.join(", "));
                }
            }
        } else if (h.kind == Hit::Transition || h.kind == Hit::TransitionEdge) {
            for (const auto& tr2 : s.transitions)
                if (tr2.id == h.id) tip = QString("<b>%1</b><br>%2").arg(transitionName(tr2.type), toQ(formatTimecode(tr2.duration, fps)));
        } else if (h.kind == Hit::FadeIn || h.kind == Hit::FadeOut) {
            tip = tr("Drag to set the fade");
        } else if (h.kind == Hit::GainLine) {
            tip = tr("Drag to change the clip volume, Ctrl+click to add a keyframe");
        }
        if (tip.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(he->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(e);
}

void TimelineWidget::leaveEvent(QEvent*) {
    if (mode_ == Mode::None) unsetCursor();
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && mode_ != Mode::None) {
        if (s_->dragging()) s_->endDrag(false);
        mode_ = Mode::None;
        update();
        return;
    }
    QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------------------------------------
// Context menus

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) {
    const QPointF pos = e->pos();
    QMenu m(this);
    if (pos.x() < bodyLeft() && pos.y() >= rulerH_) {
        int ri = rowAt(int(pos.y()));
        if (ri < 0) return;
        const Row r = rows_[size_t(ri)];
        m.addAction(tr("Add Video Track"), this, [this] {
            s_->edit(tr("Add Track"), [](Project& p) { edit::addTrack(p.sequence, TrackKind::Video); });
        });
        m.addAction(tr("Add Audio Track"), this, [this] {
            s_->edit(tr("Add Track"), [](Project& p) { edit::addTrack(p.sequence, TrackKind::Audio); });
        });
        if (r.kind != TrackKind::Caption)
            m.addAction(tr("Delete Track"), this, [this, r] {
                bool ok = false;
                s_->edit(tr("Delete Track"), [&](Project& p) { ok = edit::deleteTrack(p.sequence, r.track); });
                if (!ok) emit s_->message(tr("A sequence needs at least one track of each kind."));
            });
        m.addSeparator();
        for (auto [label, hgt] : {std::pair{tr("Small Height"), 34}, {tr("Medium Height"), 56}, {tr("Large Height"), 96}}) {
            int hh = r.kind == TrackKind::Caption ? std::min(hgt, 48) : hgt;
            m.addAction(label, this, [this, r, hh] {
                s_->edit(tr("Track Height"), [&](Project& p) {
                    if (Track* t = p.sequence.track(r.track)) t->height = hh;
                });
            });
        }
        m.addAction(tr("All Tracks: Medium Height"), this, [this] {
            s_->edit(tr("Track Height"), [](Project& p) {
                for (auto& t : p.sequence.tracks) t.height = t.kind == TrackKind::Caption ? 32 : 56;
            });
        });
        m.exec(e->globalPos());
        return;
    }
    if (pos.y() < rulerH_) {
        Tick t = xToTime(pos.x());
        for (const auto& mk : s_->seq().markers)
            if (std::abs(timeToX(mk.t) - pos.x()) < 6) {
                Id id = mk.id;
                m.addAction(tr("Delete Marker"), this, [this, id] {
                    s_->edit(tr("Delete Marker"), [&](Project& p) {
                        auto& v = p.sequence.markers;
                        v.erase(std::remove_if(v.begin(), v.end(), [&](const Marker& x) { return x.id == id; }), v.end());
                    });
                });
            }
        m.addAction(tr("Add Marker Here"), this, [this, t] {
            s_->edit(tr("Add Marker"), [&](Project& p) { edit::addMarker(p.sequence, s_->snapToFrame(t)); });
        });
        m.addAction(tr("Clear In/Out"), this, [this] {
            s_->edit(tr("Clear In/Out"), [](Project& p) { p.sequence.inPoint = p.sequence.outPoint = kInvalidTick; });
        });
        m.exec(e->globalPos());
        return;
    }
    Hit h = hitTest(pos);
    if (h.kind == Hit::Transition || h.kind == Hit::TransitionEdge) {
        s_->setSelectedTransition(h.id);
        Id id = h.id;
        QMenu* align = m.addMenu(tr("Alignment"));
        for (auto [label, a] : {std::pair{tr("Center at Cut"), TransitionAlign::Center},
                                {tr("Start at Cut"), TransitionAlign::Start}, {tr("End at Cut"), TransitionAlign::End}})
            align->addAction(label, this, [this, id, a] {
                s_->edit(tr("Transition Alignment"), [&](Project& p) {
                    if (Transition* t = p.sequence.transition(id)) t->align = a;
                });
            });
        m.addAction(tr("Set Duration..."), this, [this, id] {
            const Transition* t = s_->doc().project.sequence.transition(id);
            if (!t) return;
            bool ok = false;
            double secs = QInputDialog::getDouble(this, tr("Transition Duration"), tr("Seconds:"),
                                                  ticksToSeconds(t->duration), 0.02, 60, 2, &ok);
            if (ok) s_->edit(tr("Transition Duration"), [&](Project& p) {
                    if (Transition* x = p.sequence.transition(id)) x->duration = s_->snapToFrame(secondsToTicks(secs));
                });
        });
        m.addAction(tr("Delete"), this, [this] { s_->deleteSelection(false); });
        m.exec(e->globalPos());
        return;
    }
    if (h.kind == Hit::Caption || h.kind == Hit::CaptionHead || h.kind == Hit::CaptionTail) {
        Id id = h.id;
        if (std::find(s_->selectedCaptions.begin(), s_->selectedCaptions.end(), id) == s_->selectedCaptions.end())
            s_->selectedCaptions = {id};
        m.addAction(tr("Split at Playhead"), this, [this, id] {
            s_->edit(tr("Split Caption"), [&](Project& p) { edit::splitCaption(p.sequence, id, s_->playhead()); });
        });
        m.addAction(tr("Merge with Next"), this, [this, id] {
            s_->edit(tr("Merge Captions"), [&](Project& p) { edit::mergeCaptionWithNext(p.sequence, id); });
        });
        m.addAction(tr("Delete"), this, [this] { s_->deleteSelection(false); });
        m.exec(e->globalPos());
        return;
    }
    if (h.kind == Hit::Clip || h.kind == Hit::ClipHead || h.kind == Hit::ClipTail || h.kind == Hit::GainLine ||
        h.kind == Hit::FadeIn || h.kind == Hit::FadeOut) {
        if (!s_->isSelected(h.id)) s_->setSelection({h.id});
        const Clip* c = s_->seq().clip(h.id);
        const bool video = c && s_->seq().track(c->trackId)->kind == TrackKind::Video;
        m.addAction(tr("Cut"), QKeySequence::Cut, this, [this] { s_->cutSelection(); });
        m.addAction(tr("Copy"), QKeySequence::Copy, this, [this] { s_->copySelection(); });
        m.addAction(tr("Paste"), QKeySequence::Paste, this, [this] { s_->paste(false); });
        m.addSeparator();
        m.addAction(tr("Clear"), QKeySequence::Delete, this, [this] { s_->deleteSelection(false); });
        m.addAction(tr("Ripple Delete"), QKeySequence("Shift+Del"), this, [this] { s_->deleteSelection(true); });
        m.addSeparator();
        m.addAction(c && c->enabled ? tr("Disable") : tr("Enable"), QKeySequence("Shift+E"), this, [this] { s_->toggleEnable(); });
        m.addAction(c && c->linkId ? tr("Unlink") : tr("Link"), QKeySequence("Ctrl+L"), this, [this] { s_->toggleLink(); });
        m.addSeparator();
        QAction* speed = m.addAction(tr("Speed/Duration..."));
        speed->setShortcut(QKeySequence("Ctrl+R"));
        connect(speed, &QAction::triggered, this, [this] { emit s_->speedDialogRequested(); });
        if (video && c->kind == ClipKind::Media) {
            m.addAction(tr("Add Frame Hold"), this, [this] { s_->addFrameHold(false); });
            m.addAction(tr("Insert Frame Hold Segment"), this, [this] { s_->addFrameHold(true); });
        }
        if (video) {
            m.addSeparator();
            m.addAction(tr("Scale to Frame Size (fit)"), this, [this] { s_->setClipFit(FitMode::Fit); });
            m.addAction(tr("Scale to Fill"), this, [this] { s_->setClipFit(FitMode::Fill); });
            m.addAction(tr("Native Size (100%)"), this, [this] { s_->setClipFit(FitMode::None); });
        }
        if (c) {
            Id cid = c->id;
            QString cur = toQ(c->name);
            m.addAction(tr("Rename..."), this, [this, cid, cur] {
                bool ok = false;
                QString n = QInputDialog::getText(this, tr("Rename Clip"), tr("Name:"), QLineEdit::Normal, cur, &ok);
                if (ok && !n.isEmpty()) s_->renameClip(cid, n);
            });
            m.addAction(tr("Nest"), this, [this] { s_->nestSelection(); });
            if (!c->motion.type.empty()) {
                QMenu* am = m.addMenu(tr("Animate"));
                const QStringList names = Session::animationNames();
                for (int i = 0; i < names.size(); ++i) am->addAction(names[i], this, [this, i] { s_->animateSelection(i); });
            }
            if (c->kind == ClipKind::Nested) m.addAction(tr("Open Nested Sequence"), this, [this, cid] { s_->openNested(cid); });
            QMenu* lm = m.addMenu(tr("Label"));
            for (const auto& lc : kLabelColors) {
                const uint32_t argb = lc.argb;
                QAction* la = lm->addAction(swatchIcon(argb), QCoreApplication::translate("Label", lc.name), this,
                                            [this, argb] { s_->setSelectedLabel(argb); });
                la->setCheckable(true);
                la->setChecked(c->label == argb);
            }
            lm->addSeparator();
            lm->addAction(tr("Default"), this, [this] { s_->setSelectedLabel(0); });
        }
        if (c && c->kind == ClipKind::Media && s_->seq().track(c->trackId)->kind == TrackKind::Audio) {
            m.addAction(tr("Audio Gain..."), this, [this] { emit s_->gainDialogRequested(); });
            m.addAction(tr("Normalize to -14 LUFS"), this, [this] { s_->normalizeSelectedAudio(-14.0); });
        }
        if (c && c->kind == ClipKind::Media) {
            m.addSeparator();
            Id asset = c->assetId;
            m.addAction(tr("Open in Source Monitor"), this, [this, asset] { s_->openInSource(asset); });
        }
        if (c && !c->effects.empty()) {
            Id id = c->id;
            m.addAction(tr("Remove Effects"), this, [this, id] {
                s_->edit(tr("Remove Effects"), [&](Project& p) {
                    if (Clip* x = p.sequence.clip(id)) x->effects.clear();
                });
            });
        }
        m.exec(e->globalPos());
        return;
    }
    m.addAction(tr("Paste"), this, [this] { s_->paste(false); });
    if (s_->gapTrack) m.addAction(tr("Ripple Delete Gap"), this, [this] { s_->deleteSelection(true); });
    m.addAction(tr("Add Marker at Playhead"), this, [this] { s_->addMarkerAtPlayhead(); });
    m.exec(e->globalPos());
}

// ---------------------------------------------------------------------------------------------
// Drag & drop from the Project and Effects panels

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* md = e->mimeData();
    dropSource_ = {};
    dropWasEmpty_ = s_->seq().clips.empty();
    if (md->hasUrls() && !md->hasFormat(kMimeAssets)) {
        for (const QUrl& u : md->urls())
            if (u.isLocalFile() && isSupportedExtension(toS(u.toLocalFile()))) {
                e->acceptProposedAction();
                return;
            }
        return;
    }
    if (md->hasFormat(kMimeSource)) {
        QStringList f = QString::fromUtf8(md->data(kMimeSource)).split(';');
        if (f.size() == 5) {
            dropAssets_ = {Id(f[0].toULongLong())};
            dropSource_ = {f[1].toLongLong(), f[2].toLongLong(), f[3] == "1", f[4] == "1", true};
            s_->beginDrag(tr("Add Clips"));
            mode_ = Mode::DropAssets;
            e->acceptProposedAction();
            dragMoveEvent(static_cast<QDragMoveEvent*>(e));
        }
        return;
    }
    if (md->hasFormat(kMimeAssets)) {
        dropAssets_.clear();
        for (const QString& s : QString::fromUtf8(md->data(kMimeAssets)).split(',', Qt::SkipEmptyParts))
            dropAssets_.push_back(Id(s.toULongLong()));
        if (dropAssets_.empty()) return;
        s_->beginDrag(tr("Add Clips"));
        mode_ = Mode::DropAssets;
        e->acceptProposedAction();
        dragMoveEvent(static_cast<QDragMoveEvent*>(e));
        return;
    }
    if (md->hasFormat(kMimeEffect) || md->hasFormat(kMimeTransition) || md->hasFormat(kMimePreset) || md->hasFormat(kMimeSequence))
        e->acceptProposedAction();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    const QPointF pos = e->position();
    if (mode_ == Mode::DropAssets) {
        const Tick t = snap(xToTime(pos.x()), {});
        int ri = rowAt(int(pos.y()));
        int vTrack = s_->targetVideo, aTrack = s_->targetAudio;
        if (ri >= 0) {
            const Row& r = rows_[size_t(ri)];
            if (r.kind == TrackKind::Video) vTrack = r.index;
            if (r.kind == TrackKind::Audio) aTrack = r.index;
        } else if (pos.y() >= rulerH_ && !rows_.empty() && pos.y() - rulerH_ + scrollY_ < rows_.front().y) {
            vTrack = int(s_->seq().tracksOf(TrackKind::Video).size());  // above the top: new video track
        }
        const bool insert = e->modifiers() & Qt::ControlModifier;
        auto ids = dropAssets_;
        const DropSource src = dropSource_;
        s_->updateDrag([&](Project& p) {
            Tick at = t;
            for (Id aid : ids) {
                const Asset* a = p.asset(aid);
                if (!a) continue;
                Asset copy = *a;
                Tick dur = copy.info.isImage ? kDefaultStillDuration : copy.info.duration;
                Tick in = src.valid ? src.in : 0, out = src.valid ? src.out : dur;
                auto clips = edit::clipsForAsset(p.sequence, copy, at, in, out, vTrack, aTrack, !src.valid || src.video,
                                                 !src.valid || src.audio);
                Tick end = at;
                for (const auto& c : clips) end = std::max(end, c.end());
                if (insert) edit::insertClips(p.sequence, clips, at);
                else edit::overwriteClips(p.sequence, clips);
                at = end;
            }
        });
        e->acceptProposedAction();
        return;
    }
    e->acceptProposedAction();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
    if (mode_ == Mode::DropAssets) {
        s_->endDrag(false);
        mode_ = Mode::None;
    }
}

void TimelineWidget::dropEvent(QDropEvent* e) {
    const QMimeData* md = e->mimeData();
    if (mode_ == Mode::DropAssets) {
        s_->endDrag(true);
        mode_ = Mode::None;
        snapShown_ = false;
        e->acceptProposedAction();
        if (!dropAssets_.empty()) s_->checkSequenceMatch(dropAssets_.front(), dropWasEmpty_);
        return;
    }
    if (md->hasUrls() && !md->hasFormat(kMimeAssets)) {
        QStringList files;
        for (const QUrl& u : md->urls())
            if (u.isLocalFile() && isSupportedExtension(toS(u.toLocalFile()))) files << u.toLocalFile();
        if (files.isEmpty()) return;
        const Tick t = snap(xToTime(e->position().x()), {});
        int vTrack = s_->targetVideo, aTrack = s_->targetAudio;
        int ri = rowAt(int(e->position().y()));
        if (ri >= 0) {
            const Row& r = rows_[size_t(ri)];
            if (r.kind == TrackKind::Video) vTrack = r.index;
            if (r.kind == TrackKind::Audio) aTrack = r.index;
        }
        s_->importAndPlace(files, t, vTrack, aTrack, e->modifiers() & Qt::ControlModifier);
        e->acceptProposedAction();
        return;
    }
    Hit h = hitTest(e->position());
    if (md->hasFormat(kMimeSequence)) {
        // nest the dragged sequence at the drop point (video on the row's / target video track)
        const Id uid = Id(md->data(kMimeSequence).toULongLong());
        const Tick t = snap(xToTime(e->position().x()), {});
        int vTrack = s_->targetVideo, aTrack = s_->targetAudio;
        if (int ri = rowAt(int(e->position().y())); ri >= 0) {
            const Row& r = rows_[size_t(ri)];
            if (r.kind == TrackKind::Video) vTrack = r.index;
            if (r.kind == TrackKind::Audio) aTrack = r.index;
        }
        s_->placeSequence(uid, t, vTrack, aTrack, e->modifiers() & Qt::ControlModifier);
        e->acceptProposedAction();
        return;
    }
    if (md->hasFormat(kMimePreset)) {
        Effect fx;
        if (h.kind != Hit::Clip && h.kind != Hit::ClipHead && h.kind != Hit::ClipTail) return;
        if (!effectFromJson(md->data(kMimePreset).toStdString(), fx)) return;
        if (s_->applyEffect({h.id}, fx, tr("Apply Preset"))) s_->setSelection({h.id});
        else emit s_->message(tr("The preset does not fit that clip."));
        e->acceptProposedAction();
        return;
    }
    if (md->hasFormat(kMimeEffect)) {
        std::string type = toS(QString::fromUtf8(md->data(kMimeEffect)));
        if (h.kind != Hit::Clip && h.kind != Hit::ClipHead && h.kind != Hit::ClipTail) return;
        Id id = h.id;
        const EffectDef* def = findEffectDef(type);
        const TrackKind want = def && !def->video ? TrackKind::Audio : TrackKind::Video;
        bool added = s_->edit(tr("Add Effect"), [&](Project& p) {
            Clip* c = p.sequence.clip(id);
            if (!c || p.sequence.track(c->trackId)->kind != want) return;
            for (const auto& ef : c->effects)
                if (ef.type == type) return;  // one instance per type
            c->effects.push_back(makeEffect(type));
        });
        if (added) s_->setSelection({id});
        else emit s_->message(want == TrackKind::Audio ? tr("That effect is already on the clip (or the clip is not audio).")
                                                       : tr("That effect is already on the clip (or the clip is not video)."));
        e->acceptProposedAction();
        return;
    }
    if (md->hasFormat(kMimeTransition)) {
        int ri = rowAt(int(e->position().y()));
        if (ri < 0) return;
        const Row& r = rows_[size_t(ri)];
        const auto type = TransitionType(QString::fromUtf8(md->data(kMimeTransition)).toInt());
        const Tick t = xToTime(e->position().x());
        const Tick thr = Tick(30.0 / pps_ * kTicksPerSecond);
        auto clips = s_->seq().clipsOnTrack(r.track);
        Id a = 0, b = 0;
        Tick best = thr;
        for (size_t i = 0; i < clips.size(); ++i) {
            if (std::llabs(clips[i]->start - t) < best) {
                best = std::llabs(clips[i]->start - t);
                b = clips[i]->id;
                a = (i > 0 && clips[i - 1]->end() == clips[i]->start) ? clips[i - 1]->id : 0;
            }
            if (std::llabs(clips[i]->end() - t) < best && !(i + 1 < clips.size() && clips[i + 1]->start == clips[i]->end())) {
                best = std::llabs(clips[i]->end() - t);
                a = clips[i]->id;
                b = 0;
            }
        }
        if (!a && !b) {
            emit s_->message(tr("Drop transitions on a cut or a clip edge."));
            return;
        }
        Id trackId = r.track;
        s_->edit(tr("Add Transition"), [&](Project& p) {
            edit::addTransition(p.sequence, trackId, a, b, type, kTicksPerSecond, TransitionAlign::Center);
        });
        e->acceptProposedAction();
    }
}

} // namespace cut::ui
