#include "ui/video_widget.h"

#include "core/edits.h"
#include "engine/frame_renderer.h"
#include "ui/main_window.h"
#include "ui/session.h"

#include <QInputDialog>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>

namespace cut::ui {

VideoWidget::VideoWidget(Session* s, int viewer, bool program, QWidget* parent)
    : QWidget(parent), s_(s), viewer_(viewer), program_(program) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setMinimumSize(160, 90);
    setFocusPolicy(Qt::ClickFocus);
    if (program_) {
        connect(s_, &Session::selectionChanged, this, &VideoWidget::refreshHandles);
        connect(s_, &Session::changed, this, &VideoWidget::refreshHandles);
        connect(s_, &Session::playheadChanged, this, [this] {
            if (!s_->playing()) refreshHandles();
        });
    }
    connect(s_, &Session::engineRestarted, this, [this] {
        attached_ = false;
        attach();
        setView(view_);
    });
}

VideoWidget::~VideoWidget() {
    if (attached_ && s_->engine()) s_->engine()->detachWindow(viewer_);
}

void VideoWidget::attach() {
    if (!s_->engine() || !isVisible()) return;
    HWND h = reinterpret_cast<HWND>(winId());
    const double dpr = devicePixelRatioF();
    if (attached_ && h == hwnd_) return;
    hwnd_ = h;
    s_->engine()->attachWindow(viewer_, h, int(width() * dpr), int(height() * dpr));
    attached_ = true;
}

void VideoWidget::showEvent(QShowEvent*) { attach(); }

void VideoWidget::hideEvent(QHideEvent*) {
    if (attached_ && s_->engine()) s_->engine()->detachWindow(viewer_);
    attached_ = false;
}

void VideoWidget::resizeEvent(QResizeEvent*) {
    if (!s_->engine()) return;
    if (!attached_) {
        attach();
        return;
    }
    const double dpr = devicePixelRatioF();
    s_->engine()->resizeWindow(viewer_, int(width() * dpr), int(height() * dpr));
}

bool VideoWidget::event(QEvent* e) {
    if (e->type() == QEvent::WinIdChange) {
        // docking/undocking recreates the native window: rebuild the swapchain on the new HWND
        if (attached_ && s_->engine()) s_->engine()->detachWindow(viewer_);
        attached_ = false;
        if (isVisible()) attach();
    }
    return QWidget::event(e);
}

void VideoWidget::setView(const ViewSettings& v) {
    view_ = v;
    if (s_->engine()) s_->engine()->setView(viewer_, view_);
}

double VideoWidget::scale() const {
    const auto& st = program_ ? s_->seq().settings : SequenceSettings{};
    const int W = st.width, H = st.height;
    const double dpr = devicePixelRatioF();
    if (view_.zoom > 0) return view_.zoom / dpr;
    return std::min(double(width()) / W, double(height()) / H);
}

QPointF VideoWidget::toSeq(QPointF p) const {
    const auto& st = s_->seq().settings;
    const double s = scale();
    const double dpr = devicePixelRatioF();
    double x0 = (width() - st.width * s) / 2 + (view_.zoom > 0 ? view_.panX / dpr : 0);
    double y0 = (height() - st.height * s) / 2 + (view_.zoom > 0 ? view_.panY / dpr : 0);
    return {(p.x() - x0) / s, (p.y() - y0) / s};
}

QPointF VideoWidget::toWidget(QPointF q) const {
    const auto& st = s_->seq().settings;
    const double s = scale();
    const double dpr = devicePixelRatioF();
    double x0 = (width() - st.width * s) / 2 + (view_.zoom > 0 ? view_.panX / dpr : 0);
    double y0 = (height() - st.height * s) / 2 + (view_.zoom > 0 ? view_.panY / dpr : 0);
    return {x0 + q.x() * s, y0 + q.y() * s};
}

bool VideoWidget::cornersOf(Id clip, Corners& out) const {
    const Clip* c = s_->seq().clip(clip);
    if (!c) return false;
    SnapshotPtr snap = s_->snapshot();
    return FrameRenderer::clipCorners(*snap, *c, std::clamp(s_->playhead(), c->start, c->end() - 1), out);
}

static bool inQuad(const Corners& c, QPointF p) {
    // corners TL, TR, BL, BR -> polygon TL, TR, BR, BL
    const int order[4] = {0, 1, 3, 2};
    bool inside = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        double xi = c.p[order[i]][0], yi = c.p[order[i]][1], xj = c.p[order[j]][0], yj = c.p[order[j]][1];
        if (((yi > p.y()) != (yj > p.y())) && (p.x() < (xj - xi) * (p.y() - yi) / (yj - yi + 1e-12) + xi)) inside = !inside;
    }
    return inside;
}

static const Effect* maskOf(const Clip& c) {
    for (const auto& e : c.effects)
        if (e.type == fx::Mask && e.enabled) return &e;
    return nullptr;
}

bool VideoWidget::maskQuad(const Clip& c, Corners& out) const {
    const Effect* m = maskOf(c);
    Corners q;
    if (!m || !cornersOf(c.id, q)) return false;
    const Tick local = std::clamp(s_->playhead(), c.start, c.end() - 1) - c.start;
    const double cx = m->p("cx", local) / 100, cy = m->p("cy", local) / 100;
    const double hw = std::max(0.0, m->p("w", local)) / 200, hh = std::max(0.0, m->p("h", local)) / 200;
    // image (u, v) -> sequence pixels through the clip's corners
    auto at = [&](double u, double v, float* o) {
        o[0] = float(q.p[0][0] + u * (q.p[1][0] - q.p[0][0]) + v * (q.p[2][0] - q.p[0][0]));
        o[1] = float(q.p[0][1] + u * (q.p[1][1] - q.p[0][1]) + v * (q.p[2][1] - q.p[0][1]));
    };
    at(cx - hw, cy - hh, out.p[0]);
    at(cx + hw, cy - hh, out.p[1]);
    at(cx - hw, cy + hh, out.p[2]);
    at(cx + hw, cy + hh, out.p[3]);
    return true;
}

// sequence-pixel offset -> image (u, v) offset for a clip with corners q
static QPointF toImageDelta(const Corners& q, QPointF d) {
    const double ax = q.p[1][0] - q.p[0][0], ay = q.p[1][1] - q.p[0][1];  // u axis
    const double bx = q.p[2][0] - q.p[0][0], by = q.p[2][1] - q.p[0][1];  // v axis
    const double det = ax * by - ay * bx;
    if (std::abs(det) < 1e-9) return {};
    return {(d.x() * by - d.y() * bx) / det, (ax * d.y() - ay * d.x()) / det};
}

Id VideoWidget::hitClip(QPointF seqPt) const {
    // topmost visible video clip under the point at the playhead
    const Sequence& seq = s_->seq();
    auto layers = evaluateVideo(seq, s_->playhead());
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const Clip* c = it->a.clip ? it->a.clip : it->b.clip;
        if (!c) continue;
        Corners q;
        if (cornersOf(c->id, q) && inQuad(q, seqPt)) return c->id;
    }
    return 0;
}

void VideoWidget::refreshHandles() {
    if (!program_) return;
    Id id = s_->primaryClip();
    const Clip* c = id ? s_->seq().clip(id) : nullptr;
    ViewSettings v = view_;
    v.handles = false;
    if (c && c->contains(s_->playhead())) {
        const Track* t = s_->seq().track(c->trackId);
        Corners q;
        Corners mq;
        if (t && t->kind == TrackKind::Video && c->kind == ClipKind::Adjustment && maskQuad(*c, mq)) {
            // a masked adjustment layer (Blur Region): the handles edit the mask
            v.handles = true;
            v.handleCorners = mq;
            v.anchorX = (mq.p[0][0] + mq.p[3][0]) / 2;
            v.anchorY = (mq.p[0][1] + mq.p[3][1]) / 2;
        } else if (t && t->kind == TrackKind::Video && cornersOf(id, q)) {
            v.handles = true;
            v.handleCorners = q;
            MotionValues m = motionAt(*c, s_->playhead() - c->start);
            v.anchorX = s_->seq().settings.width / 2.0 + m.posX;
            v.anchorY = s_->seq().settings.height / 2.0 + m.posY;
        }
    }
    if (v.handles != view_.handles || std::memcmp(&v.handleCorners, &view_.handleCorners, sizeof(Corners)) != 0 ||
        v.anchorX != view_.anchorX || v.anchorY != view_.anchorY)
        setView(v);
}

void VideoWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && s_->tool() == Session::Tool::Hand)) {
        drag_ = Drag::Pan;
        pressPos_ = e->position();
        startA_ = view_.panX;
        startB_ = view_.panY;
        return;
    }
    if (!program_ || e->button() != Qt::LeftButton) return;
    QPointF sp = toSeq(e->position());
    const Tick ph = s_->playhead();
    if (s_->tool() == Session::Tool::Text) {
        // new title where clicked, on the top free video track at the playhead
        const auto& st = s_->seq().settings;
        double px = sp.x() - st.width / 2.0, py = sp.y() - st.height / 2.0;
        Id newId = 0;
        s_->edit(tr("New Title"), [&](Project& p) {
            Sequence& s = p.sequence;
            Tick dur = kDefaultStillDuration;
            const Track* target = nullptr;
            auto vts = s.tracksOf(TrackKind::Video);
            for (auto it = vts.rbegin(); it != vts.rend(); ++it) {
                bool free = true;
                for (const auto* c : s.clipsOnTrack((*it)->id))
                    if (c->start < ph + dur && c->end() > ph) free = false;
                if (free && !(*it)->locked) target = *it;
                else if (target) break;
            }
            Id tid = target ? target->id : edit::addTrack(s, TrackKind::Video);
            Clip c = makeTextClip(tid, ph, dur, "Title");
            c.motion.params["posX"].value = std::round(px);
            c.motion.params["posY"].value = std::round(py);
            newId = edit::overwriteClips(s, {c}).front();
        });
        s_->setSelection({newId});
        s_->setTool(Session::Tool::Select);
        return;
    }
    Id sel = s_->primaryClip();
    Corners q;
    const Clip* sc = sel ? s_->seq().clip(sel) : nullptr;
    // Shape Mask: an adjustment layer's mask, or any clip's with Ctrl held
    Corners mq;
    if (sc && sc->contains(ph) && (sc->kind == ClipKind::Adjustment || (e->modifiers() & Qt::ControlModifier)) &&
        maskQuad(*sc, mq) && cornersOf(sel, q)) {
        const Effect* m = maskOf(*sc);
        const Tick local = ph - sc->start;
        for (int i = 0; i < 4 && drag_ == Drag::None; ++i) {
            const QPointF hp = toWidget({mq.p[i][0], mq.p[i][1]});
            if (std::hypot(hp.x() - e->position().x(), hp.y() - e->position().y()) < 9) drag_ = Drag::MaskSize;
        }
        if (drag_ == Drag::None && inQuad(mq, sp)) drag_ = Drag::MaskMove;
        if (drag_ != Drag::None) {
            dragClip_ = sel;
            pressPos_ = e->position();
            startA_ = m->p("cx", local);
            startB_ = m->p("cy", local);
            startScale_ = m->p("w", local);
            startRot_ = m->p("h", local);
            s_->beginDrag(drag_ == Drag::MaskMove ? tr("Move Mask") : tr("Resize Mask"));
            return;
        }
    }
    if (sc && sc->contains(ph) && cornersOf(sel, q)) {
        // handles first (corners -> scale), near-outside corners -> rotate, inside -> move
        MotionValues m = motionAt(*sc, ph - sc->start);
        anchorScreen_ = toWidget({s_->seq().settings.width / 2.0 + m.posX, s_->seq().settings.height / 2.0 + m.posY});
        for (int i = 0; i < 4; ++i) {
            QPointF hp = toWidget({q.p[i][0], q.p[i][1]});
            double d = std::hypot(hp.x() - e->position().x(), hp.y() - e->position().y());
            if (d < 9) {
                drag_ = Drag::Scale;
            } else if (d < 28 && !inQuad(q, sp)) {
                drag_ = Drag::Rotate;
            }
            if (drag_ != Drag::None) break;
        }
        if (drag_ == Drag::None && inQuad(q, sp)) drag_ = Drag::Move;
        if (drag_ != Drag::None) {
            dragClip_ = sel;
            pressPos_ = e->position();
            startA_ = m.posX;
            startB_ = m.posY;
            startScale_ = m.scale;
            startRot_ = m.rotation;
            s_->beginDrag(drag_ == Drag::Move ? tr("Move") : drag_ == Drag::Scale ? tr("Scale") : tr("Rotate"));
            return;
        }
    }
    Id hit = hitClip(sp);
    s_->setSelection(hit ? std::vector<Id>{hit} : std::vector<Id>{});
}

void VideoWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Drag::Pan) {
        const double dpr = devicePixelRatioF();
        if (view_.zoom > 0) {
            ViewSettings v = view_;
            v.panX = startA_ + (e->position().x() - pressPos_.x()) * dpr;
            v.panY = startB_ + (e->position().y() - pressPos_.y()) * dpr;
            setView(v);
        }
        return;
    }
    if (drag_ == Drag::None || !dragClip_) {
        // cursor feedback
        if (program_ && s_->tool() == Session::Tool::Text) setCursor(Qt::IBeamCursor);
        else if (s_->tool() == Session::Tool::Hand) setCursor(Qt::OpenHandCursor);
        else unsetCursor();
        return;
    }
    const Clip* c = s_->seq().clip(dragClip_);
    if (!c) return;
    const Tick local = s_->playhead() - c->start;
    const double s = scale();
    QPointF d = e->position() - pressPos_;
    const Id id = dragClip_;
    auto setParam = [local](Clip& cl, const char* name, double v) { cl.motion.params[name].set(local, v); };
    if (drag_ == Drag::MaskMove || drag_ == Drag::MaskSize) {
        Corners q;
        if (!cornersOf(id, q)) return;
        auto setMask = [local](Clip& cl, const char* name, double v) {
            for (auto& ef : cl.effects)
                if (ef.type == fx::Mask && ef.enabled) {
                    ef.params[name].set(local, v);
                    return;
                }
        };
        if (drag_ == Drag::MaskMove) {
            const QPointF du = toImageDelta(q, d / s);
            s_->updateDrag([&](Project& p) {
                if (Clip* cl = p.sequence.clip(id)) {
                    setMask(*cl, "cx", std::round((startA_ + du.x() * 100) * 10) / 10);
                    setMask(*cl, "cy", std::round((startB_ + du.y() * 100) * 10) / 10);
                }
            });
        } else {
            // the corner follows the pointer; the size grows symmetrically around the centre
            const QPointF sp = toSeq(e->position());
            const QPointF uv = toImageDelta(q, sp - QPointF(q.p[0][0], q.p[0][1]));
            const double w = std::clamp(2 * std::abs(uv.x() * 100 - startA_), 0.5, 200.0);
            const double h = std::clamp(2 * std::abs(uv.y() * 100 - startB_), 0.5, 200.0);
            s_->updateDrag([&](Project& p) {
                if (Clip* cl = p.sequence.clip(id)) {
                    setMask(*cl, "w", std::round(w * 10) / 10);
                    setMask(*cl, "h", std::round(h * 10) / 10);
                }
            });
        }
        return;
    }
    if (drag_ == Drag::Move) {
        double nx = startA_ + d.x() / s, ny = startB_ + d.y() / s;
        if (e->modifiers() & Qt::ShiftModifier) {
            if (std::abs(d.x()) > std::abs(d.y())) ny = startB_;
            else nx = startA_;
        }
        s_->updateDrag([&](Project& p) {
            if (Clip* cl = p.sequence.clip(id)) {
                setParam(*cl, "posX", std::round(nx));
                setParam(*cl, "posY", std::round(ny));
            }
        });
    } else if (drag_ == Drag::Scale) {
        double d0 = std::hypot(pressPos_.x() - anchorScreen_.x(), pressPos_.y() - anchorScreen_.y());
        double d1 = std::hypot(e->position().x() - anchorScreen_.x(), e->position().y() - anchorScreen_.y());
        double ns = std::clamp(startScale_ * d1 / std::max(1.0, d0), 1.0, 1000.0);
        s_->updateDrag([&](Project& p) {
            if (Clip* cl = p.sequence.clip(id)) setParam(*cl, "scale", std::round(ns * 10) / 10);
        });
    } else if (drag_ == Drag::Rotate) {
        double a0 = std::atan2(pressPos_.y() - anchorScreen_.y(), pressPos_.x() - anchorScreen_.x());
        double a1 = std::atan2(e->position().y() - anchorScreen_.y(), e->position().x() - anchorScreen_.x());
        double nr = startRot_ + (a1 - a0) * 180.0 / 3.14159265358979;
        if (e->modifiers() & Qt::ShiftModifier) nr = std::round(nr / 15) * 15;
        s_->updateDrag([&](Project& p) {
            if (Clip* cl = p.sequence.clip(id)) setParam(*cl, "rotation", std::round(nr * 10) / 10);
        });
    }
}

void VideoWidget::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ != Drag::None && drag_ != Drag::Pan) s_->endDrag(true);
    drag_ = Drag::None;
    dragClip_ = 0;
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!program_) return;
    Id hit = hitClip(toSeq(e->position()));
    const Clip* c = hit ? s_->seq().clip(hit) : nullptr;
    if (!c || c->kind != ClipKind::Text) return;
    bool ok = false;
    QString t = QInputDialog::getMultiLineText(this, tr("Edit Title"), tr("Text:"), toQ(c->text), &ok);
    if (ok) s_->edit(tr("Edit Title Text"), [&](Project& p) {
            if (Clip* x = p.sequence.clip(hit)) x->text = toS(t);
        });
}

void VideoWidget::contextMenuEvent(QContextMenuEvent* e) {
    if (!program_) return;
    const QPointF sp = toSeq(e->pos());
    Id target = hitClip(sp);
    if (!target) target = s_->primaryClip();
    const Clip* c = target ? s_->seq().clip(target) : nullptr;
    QMenu m(this);
    for (double k : {1.5, 2.0}) {
        QAction* a = m.addAction(tr("Punch In Here (%1x)").arg(k), this, [this, target, sp, k] { s_->punchIn(target, sp.x(), sp.y(), k); });
        a->setEnabled(c && c->contains(s_->playhead()));
    }
    if (s_->canTrackMask(s_->primaryClip())) {
        m.addSeparator();
        m.addAction(tr("Track Mask Forward"), this, [this] {
            if (auto* mw = qobject_cast<MainWindow*>(window())) mw->trackMask(s_->primaryClip());
        });
    }
    m.exec(e->globalPos());
}

void VideoWidget::wheelEvent(QWheelEvent* e) {
    if (!(e->modifiers() & Qt::ControlModifier)) return;
    ViewSettings v = view_;
    double cur = v.zoom > 0 ? v.zoom : scale() * devicePixelRatioF();
    v.zoom = std::clamp(cur * (e->angleDelta().y() > 0 ? 1.25 : 0.8), 0.05, 8.0);
    setView(v);
    emit zoomChanged();
}

} // namespace cut::ui
