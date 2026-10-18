#include "ui/effect_controls.h"

#include "core/project_io.h"
#include "engine/frame_renderer.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QSettings>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace cut::ui {

// ---------------------------------------------------------------------------------------------
ScrubValue::ScrubValue(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::SizeHorCursor);
    setMinimumWidth(64);
    setFixedHeight(20);
}

void ScrubValue::setup(double mn, double mx, double step, int dec, const QString& unit) {
    min_ = mn;
    max_ = mx;
    step_ = step;
    dec_ = dec;
    unit_ = unit;
}

void ScrubValue::setValue(double v) {
    if (v == v_) return;
    v_ = v;
    update();
}

void ScrubValue::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setPen(isEnabled() ? QColor(0x4d, 0x9d, 0xff) : col::textDim);
    QFont f = font();
    f.setUnderline(dragging_);
    p.setFont(f);
    QString t = QString::number(v_, 'f', dec_) + (unit_.isEmpty() ? "" : " " + unit_);
    p.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, t);
}

void ScrubValue::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    press_ = e->globalPosition();
    start_ = v_;
    dragging_ = false;
}

void ScrubValue::mouseMoveEvent(QMouseEvent* e) {
    if (!(e->buttons() & Qt::LeftButton)) return;
    double dx = e->globalPosition().x() - press_.x();
    if (!dragging_ && std::abs(dx) < 3) return;
    if (!dragging_) {
        dragging_ = true;
        emit dragStarted();
    }
    double mult = (e->modifiers() & Qt::ShiftModifier) ? 10 : (e->modifiers() & Qt::ControlModifier) ? 0.1 : 1;
    double v = std::clamp(start_ + dx * step_ * mult, min_, max_);
    double q = std::pow(10.0, dec_);
    v = std::round(v * q) / q;
    v_ = v;
    update();
    emit dragged(v);
}

void ScrubValue::mouseReleaseEvent(QMouseEvent*) {
    if (dragging_) {
        dragging_ = false;
        update();
        emit dragFinished();
        return;
    }
    // click: type a value
    auto* ed = new QLineEdit(QString::number(v_, 'f', dec_), this);
    ed->setGeometry(rect());
    ed->selectAll();
    ed->show();
    ed->setFocus();
    connect(ed, &QLineEdit::editingFinished, this, [this, ed] {
        bool ok = false;
        double v = ed->text().toDouble(&ok);
        ed->deleteLater();
        if (ok) {
            v_ = std::clamp(v, min_, max_);
            update();
            emit edited(v_);
        }
    });
}

// ---------------------------------------------------------------------------------------------
KeyLane::KeyLane(Session* s, Id clip, std::function<const Param*()> get, QWidget* parent)
    : QWidget(parent), s_(s), clip_(clip), get_(std::move(get)) {
    setMinimumWidth(80);
    setFixedHeight(20);
    setToolTip(tr("Keyframes - drag to move, right-click for interpolation, double-click for the value graph"));
}

void KeyLane::setGraph(bool on) {
    graph_ = on;
    setFixedHeight(on ? 84 : 20);
    fitRange();
    update();
}

double KeyLane::xOf(Tick local) const {
    const Clip* c = s_->seq().clip(clip_);
    if (!c || c->duration <= 0) return 0;
    return 4 + (width() - 8) * double(local) / double(c->duration);
}

Tick KeyLane::localAt(double x) const {
    const Clip* c = s_->seq().clip(clip_);
    if (!c) return 0;
    return s_->snapToFrame(Tick(std::clamp((x - 4) / std::max(1.0, width() - 8.0), 0.0, 1.0) * double(c->duration)));
}

double KeyLane::yOf(double v) const { return 6 + (height() - 12) * (1.0 - (v - vmin_) / std::max(1e-9, vmax_ - vmin_)); }
double KeyLane::valueAt(double y) const { return vmin_ + (1.0 - (y - 6) / std::max(1.0, height() - 12.0)) * (vmax_ - vmin_); }

void KeyLane::fitRange() {
    // the curve's own range, padded, so small animations still fill the graph
    const Param* prm = get_();
    const Clip* c = s_->seq().clip(clip_);
    if (!prm || !c || !prm->animated()) return;
    double lo = prm->keys.front().v, hi = lo;
    for (int i = 0; i <= 64; ++i) {
        const double v = prm->at(c->duration * i / 64);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    for (const auto& k : prm->keys) {
        lo = std::min(lo, k.v);
        hi = std::max(hi, k.v);
    }
    const double pad = std::max((hi - lo) * 0.15, 1.0);
    vmin_ = lo - pad;
    vmax_ = hi + pad;
}

void KeyLane::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));
    const Clip* c = s_->seq().clip(clip_);
    if (!c) return;
    p.setRenderHint(QPainter::Antialiasing);
    const Param* prm = get_();
    const bool curve = graph_ && prm && prm->animated();
    if (curve) {
        if (!dragging_) fitRange();
        p.setPen(QColor(255, 255, 255, 25));
        for (int g = 1; g < 4; ++g) p.drawLine(QPointF(0, height() * g / 4.0), QPointF(width(), height() * g / 4.0));
        QPainterPath path;
        for (int x = 4; x <= width() - 4; x += 2) {
            const QPointF pt(x, yOf(prm->at(localAt(x))));
            if (x == 4) path.moveTo(pt);
            else path.lineTo(pt);
        }
        p.setPen(QPen(QColor(0x80, 0xc0, 0xff), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(QColor(0x8c, 0x8c, 0x8c));
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        p.drawText(QRectF(4, 1, width() - 8, 12), Qt::AlignLeft, QString::number(vmax_, 'g', 4));
        p.drawText(QRectF(4, height() - 13, width() - 8, 12), Qt::AlignLeft, QString::number(vmin_, 'g', 4));
    }
    if (prm)
        for (const auto& k : prm->keys) {
            const double x = xOf(k.t), y = curve ? yOf(k.v) : 10;
            QPolygonF d;
            const double r = 4.5;
            d << QPointF(x, y - r) << QPointF(x + r, y) << QPointF(x, y + r) << QPointF(x - r, y);
            p.setPen(QColor(0x10, 0x10, 0x10));
            p.setBrush(k.interp == Interp::Hold ? QColor(0xe0, 0x90, 0x40) : k.interp != Interp::Linear ? QColor(0x80, 0xc0, 0xff)
                                                                                                    : QColor(0xd8, 0xd8, 0xd8));
            p.drawPolygon(d);
        }
    double px = xOf(s_->playhead() - c->start);
    if (px >= 0 && px <= width()) {
        p.setPen(col::playhead);
        p.drawLine(QPointF(px, 0), QPointF(px, height()));
    }
}

void KeyLane::mousePressEvent(QMouseEvent* e) {
    const Param* prm = get_();
    const Clip* c = s_->seq().clip(clip_);
    if (!c) return;
    const bool curve = graph_ && prm && prm->animated();
    Tick hit = kInvalidTick;
    if (prm)
        for (const auto& k : prm->keys) {
            const double dx = xOf(k.t) - e->position().x(), dy = curve ? yOf(k.v) - e->position().y() : 0;
            if (std::abs(dx) < 6 && std::abs(dy) < 7) hit = k.t;
        }
    if (e->button() == Qt::RightButton) {  // on a key: its menu; elsewhere: copy / paste
        emit keyMenu(hit, e->globalPosition().toPoint());
        return;
    }
    pressAt_ = e->position();
    if (hit != kInvalidTick) {
        dragKey_ = hit;
        s_->setPlayhead(c->start + hit);
    } else {
        s_->setPlayhead(c->start + localAt(e->position().x()));
    }
}

void KeyLane::mouseMoveEvent(QMouseEvent* e) {
    if (dragKey_ == kInvalidTick || !(e->buttons() & Qt::LeftButton)) return;
    if (!dragging_) {
        dragging_ = true;
        fitRange();  // frozen for the drag
        emit keyMoveStarted(dragKey_);
    }
    const Param* prm = get_();
    const bool curve = graph_ && prm && prm->animated();
    // in the graph, Shift locks time (value only) - otherwise both follow the mouse
    const Tick to = curve && (e->modifiers() & Qt::ShiftModifier) ? dragKey_ : localAt(e->position().x());
    emit keyMoved(dragKey_, to, curve ? valueAt(e->position().y()) : std::nan(""));
}

void KeyLane::mouseReleaseEvent(QMouseEvent*) {
    if (dragging_) emit keyMoveFinished();
    dragging_ = false;
    dragKey_ = kInvalidTick;
    update();
}

void KeyLane::mouseDoubleClickEvent(QMouseEvent*) {
    const Param* prm = get_();
    if (prm && prm->animated()) setGraph(!graph_);
}
// ---------------------------------------------------------------------------------------------
namespace {
// Effect reference inside a clip: -1 motion, -2 opacity, -3 volume, >= 0 index into effects.
Effect* effectOf(Clip& c, int ref) {
    if (ref == -1) return &c.motion;
    if (ref == -2) return &c.opacity;
    if (ref == -3) return &c.volume;
    if (ref >= 0 && size_t(ref) < c.effects.size()) return &c.effects[size_t(ref)];
    return nullptr;
}
const Effect* effectOf(const Clip& c, int ref) { return effectOf(const_cast<Clip&>(c), ref); }

QToolButton* smallBtn(const QString& ic, const QString& tip, QWidget* parent, bool checkable = false) {
    auto* b = new QToolButton(parent);
    b->setIcon(icon(ic));
    b->setIconSize({13, 13});
    b->setFixedSize(20, 20);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCheckable(checkable);
    return b;
}

QPushButton* colorButton(uint32_t argb, QWidget* parent) {
    auto* b = new QPushButton(parent);
    b->setFixedSize(40, 18);
    QColor c = QColor::fromRgba(argb);
    b->setStyleSheet(QString("QPushButton { background: %1; border: 1px solid #555; }").arg(c.name()));
    return b;
}
} // namespace

EffectControls::EffectControls(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    header_ = new QLabel(this);
    header_->setStyleSheet("QLabel { background: #262626; padding: 5px 8px; color: #c8c8c8; }");
    v->addWidget(header_);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    v->addWidget(scroll_, 1);
    connect(s_, &Session::selectionChanged, this, &EffectControls::rebuild);
    connect(s_, &Session::changed, this, [this] {
        if (signature() != sig_) rebuild();
        else refresh();
    });
    connect(s_, &Session::playheadChanged, this, [this] {
        if (!s_->playing()) refresh();
    });
    rebuild();
}

std::string EffectControls::signature() const {
    Id id = s_->primaryClip();
    const Clip* c = id ? s_->seq().clip(id) : nullptr;
    if (!c) return "none";
    std::string sig = std::to_string(id) + ":" + std::to_string(int(c->kind));
    for (const auto& e : c->effects) sig += "|" + e.type;
    return sig;
}

void EffectControls::refresh() {
    for (auto& f : refreshers_) f();
}

void EffectControls::rebuild() {
    refreshers_.clear();
    sig_ = signature();
    clip_ = s_->primaryClip();
    if (content_) content_->deleteLater();
    content_ = new QWidget;
    auto* root = new QVBoxLayout(content_);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);
    scroll_->setWidget(content_);
    const Clip* clip = clip_ ? s_->seq().clip(clip_) : nullptr;
    if (!clip) {
        header_->setText(tr("(no clip selected)"));
        auto* l = new QLabel(tr("Select a clip in the timeline to edit its effects.\n\nDrag effects from the Effects panel onto clips."),
                             content_);
        l->setWordWrap(true);
        l->setStyleSheet("color: #8c8c8c;");
        root->addWidget(l);
        root->addStretch(1);
        return;
    }
    const Track* track = s_->seq().track(clip->trackId);
    const bool audio = track && track->kind == TrackKind::Audio;
    header_->setText(QString("<b>%1</b> &nbsp; <span style='color:#8c8c8c'>%2</span>")
                         .arg(toQ(clip->name).toHtmlEscaped(), toQ(track ? track->name : "")));
    const Id cid = clip_;
    Session* s = s_;

    // --- helpers ------------------------------------------------------------------------------
    auto localNow = [s, cid]() -> Tick {
        const Clip* c = s->seq().clip(cid);
        return c ? std::clamp<Tick>(s->playhead() - c->start, 0, c->duration - 1) : 0;
    };
    auto inClip = [s, cid] {
        const Clip* c = s->seq().clip(cid);
        return c && c->contains(s->playhead());
    };
    auto section = [&](const QString& title, QWidget** headerOut = nullptr) {
        auto* box = new QWidget(content_);
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(2);
        auto* hdr = new QWidget(box);
        hdr->setStyleSheet("background: #2b2b2b;");
        auto* hl = new QHBoxLayout(hdr);
        hl->setContentsMargins(6, 2, 4, 2);
        auto* t = new QLabel(title, hdr);
        t->setStyleSheet("font-weight: 600; background: transparent;");
        hl->addWidget(t);
        hl->addStretch(1);
        bl->addWidget(hdr);
        auto* body = new QWidget(box);
        auto* form = new QVBoxLayout(body);
        form->setContentsMargins(8, 2, 2, 4);
        form->setSpacing(1);
        bl->addWidget(body);
        root->addWidget(box);
        if (headerOut) *headerOut = hdr;
        return form;
    };

    // One keyframeable numeric parameter row.
    auto paramRow = [&](QVBoxLayout* form, int effRef, const ParamDef& def, double displayOffset) {
        auto* row = new QWidget(content_);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(3);
        auto* sw = smallBtn("stopwatch", tr("Toggle animation"), row, true);
        h->addWidget(sw);
        auto* lab = new QLabel(toQ(def.label), row);
        lab->setFixedWidth(92);
        h->addWidget(lab);
        const std::string pid = def.id;
        auto getParam = [s, cid, effRef, pid]() -> const Param* {
            const Clip* c = s->seq().clip(cid);
            if (!c) return nullptr;
            const Effect* e = effectOf(*c, effRef);
            if (!e) return nullptr;
            auto it = e->params.find(pid);
            return it == e->params.end() ? nullptr : &it->second;
        };
        auto setParam = [effRef, pid](Project& p, Id id, Tick local, double v) {
            if (Clip* c = p.sequence.clip(id))
                if (Effect* e = effectOf(*c, effRef)) e->params[pid].set(local, v);
        };
        QWidget* editor = nullptr;
        std::function<void()> refreshEditor;
        if (!def.enumLabels.empty()) {
            auto* cb = new QComboBox(row);
            for (const auto& l : def.enumLabels) cb->addItem(toQ(l));
            connect(cb, &QComboBox::activated, row, [=](int i) {
                s->edit(toQ(def.label), [&](Project& p) { setParam(p, cid, 0, double(i)); });
            });
            refreshEditor = [=] {
                const Param* prm = getParam();
                cb->blockSignals(true);
                cb->setCurrentIndex(prm ? int(prm->value) : int(def.def));
                cb->blockSignals(false);
            };
            editor = cb;
            sw->setVisible(false);
        } else if (def.isBool) {
            auto* ck = new QCheckBox(row);
            connect(ck, &QCheckBox::toggled, row, [=](bool on) {
                s->edit(toQ(def.label), [&](Project& p) { setParam(p, cid, 0, on ? 1.0 : 0.0); });
            });
            refreshEditor = [=] {
                const Param* prm = getParam();
                ck->blockSignals(true);
                ck->setChecked((prm ? prm->value : def.def) > 0.5);
                ck->blockSignals(false);
            };
            editor = ck;
            sw->setVisible(false);
        } else {
            auto* sv = new ScrubValue(row);
            sv->setup(def.min + displayOffset, def.max + displayOffset, def.step, def.decimals, toQ(def.unit));
            connect(sv, &ScrubValue::dragStarted, row, [=] { s->beginDrag(toQ(def.label)); });
            connect(sv, &ScrubValue::dragged, row, [=](double v) {
                Tick local = localNow();
                s->updateDrag([&](Project& p) { setParam(p, cid, local, v - displayOffset); });
            });
            connect(sv, &ScrubValue::dragFinished, row, [=] { s->endDrag(true); });
            connect(sv, &ScrubValue::edited, row, [=](double v) {
                Tick local = localNow();
                s->edit(toQ(def.label), [&](Project& p) { setParam(p, cid, local, v - displayOffset); });
            });
            refreshEditor = [=] {
                const Param* prm = getParam();
                sv->setValue((prm ? prm->at(localNow()) : def.def) + displayOffset);
            };
            editor = sv;
        }
        h->addWidget(editor, 1);
        auto* prev = smallBtn("prev", tr("Previous keyframe"), row);
        auto* key = smallBtn("keyhollow", tr("Add/Remove keyframe"), row);
        auto* next = smallBtn("next", tr("Next keyframe"), row);
        h->addWidget(prev);
        h->addWidget(key);
        h->addWidget(next);
        auto* lane = new KeyLane(s, cid, getParam, row);
        lane->setProperty("paramLabel", toQ(def.label));  // (tests find lanes by it)
        h->addWidget(lane, 1);
        const bool animatable = def.animatable && def.enumLabels.empty() && !def.isBool;
        prev->setVisible(animatable);
        key->setVisible(animatable);
        next->setVisible(animatable);
        lane->setVisible(animatable);
        connect(sw, &QToolButton::clicked, row, [=](bool on) {
            Tick local = localNow();
            s->edit(on ? tr("Enable Keyframes") : tr("Disable Keyframes"), [&](Project& p) {
                Clip* c = p.sequence.clip(cid);
                Effect* e = c ? effectOf(*c, effRef) : nullptr;
                if (!e) return;
                Param& prm = e->params[pid];
                if (on) prm.addKey(local, prm.at(local));
                else {
                    prm.value = prm.at(local);
                    prm.keys.clear();
                }
            });
        });
        connect(key, &QToolButton::clicked, row, [=] {
            if (!inClip()) {
                emit s->message(tr("Move the playhead over the clip to add keyframes."));
                return;
            }
            Tick local = s->snapToFrame(localNow());
            s->edit(tr("Keyframe"), [&](Project& p) {
                Clip* c = p.sequence.clip(cid);
                Effect* e = c ? effectOf(*c, effRef) : nullptr;
                if (!e) return;
                Param& prm = e->params[pid];
                const Tick tol = p.sequence.frameDur() / 2;
                if (!prm.removeKeyAt(local, tol)) prm.addKey(local, prm.at(local));
            });
        });
        auto jump = [=](int dir) {
            const Param* prm = getParam();
            const Clip* c = s->seq().clip(cid);
            if (!prm || !c) return;
            Tick now = s->playhead() - c->start, best = kInvalidTick;
            for (const auto& k : prm->keys)
                if ((dir > 0 && k.t > now && (best == kInvalidTick || k.t < best)) ||
                    (dir < 0 && k.t < now && (best == kInvalidTick || k.t > best)))
                    best = k.t;
            if (best != kInvalidTick) s->setPlayhead(c->start + best);
        };
        connect(prev, &QToolButton::clicked, row, [=] { jump(-1); });
        connect(next, &QToolButton::clicked, row, [=] { jump(1); });
        connect(lane, &KeyLane::keyMoveStarted, row, [=] { s->beginDrag(tr("Move Keyframe")); });
        connect(lane, &KeyLane::keyMoved, row, [=](Tick from, Tick to, double value) {
            s->updateDrag([&](Project& p) {
                Clip* c = p.sequence.clip(cid);
                Effect* e = c ? effectOf(*c, effRef) : nullptr;
                if (!e) return;
                Param& prm = e->params[pid];
                for (auto& k : prm.keys)
                    if (k.t == from) {
                        k.t = to;
                        if (!std::isnan(value)) k.v = std::clamp(value, def.min, def.max);
                    }
                std::sort(prm.keys.begin(), prm.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
                prm.keys.erase(std::unique(prm.keys.begin(), prm.keys.end(),
                                           [](const Keyframe& a, const Keyframe& b) { return a.t == b.t; }),
                               prm.keys.end());
            });
        });
        connect(lane, &KeyLane::keyMoveFinished, row, [=] { s->endDrag(true); });
        connect(lane, &KeyLane::keyMenu, row, [=](Tick at, const QPoint& g) {
            QMenu m;
            // keyframe clipboard: one parameter's animation, pasted at the playhead on any parameter/clip
            static std::vector<Keyframe> clip;
            const Param* cur = getParam();
            m.addAction(tr("Copy Keyframes"), [=] {
                clip = cur ? cur->keys : std::vector<Keyframe>{};
                emit s->message(tr("Copied %n keyframe(s).", "", int(clip.size())), 2500);
            })->setEnabled(cur && cur->animated());
            m.addAction(tr("Paste Keyframes at Playhead"), [=] {
                const Clip* c0 = s->seq().clip(cid);
                if (!c0 || clip.empty()) return;
                const Tick base = s->snapToFrame(s->playhead() - c0->start) - clip.front().t;
                s->edit(tr("Paste Keyframes"), [&](Project& p) {
                    Clip* c = p.sequence.clip(cid);
                    Effect* e = c ? effectOf(*c, effRef) : nullptr;
                    if (!e) return;
                    Param& prm = e->params[pid];
                    if (!prm.animated()) prm.keys.clear();
                    for (const auto& k : clip) {
                        const Tick t = base + k.t;
                        if (t >= 0 && t < c->duration) prm.addKey(t, std::clamp(k.v, def.min, def.max), k.interp);
                    }
                });
            })->setEnabled(!clip.empty() && inClip());
            if (at == kInvalidTick) {
                m.exec(g);
                return;
            }
            m.addSeparator();
            auto setInterp = [=](Interp in) {
                s->edit(tr("Keyframe Interpolation"), [&](Project& p) {
                    Clip* c = p.sequence.clip(cid);
                    Effect* e = c ? effectOf(*c, effRef) : nullptr;
                    if (!e) return;
                    for (auto& k : e->params[pid].keys)
                        if (k.t == at) k.interp = in;
                });
            };
            m.addAction(tr("Linear"), [=] { setInterp(Interp::Linear); });
            m.addAction(tr("Ease In/Out"), [=] { setInterp(Interp::Ease); });
            m.addAction(tr("Ease In"), [=] { setInterp(Interp::EaseIn); });
            m.addAction(tr("Ease Out"), [=] { setInterp(Interp::EaseOut); });
            m.addAction(tr("Hold"), [=] { setInterp(Interp::Hold); });
            m.addSeparator();
            m.addAction(tr("Delete Keyframe"), [=] {
                s->edit(tr("Delete Keyframe"), [&](Project& p) {
                    Clip* c = p.sequence.clip(cid);
                    Effect* e = c ? effectOf(*c, effRef) : nullptr;
                    if (e) e->params[pid].removeKeyAt(at);
                });
            });
            m.exec(g);
        });
        auto refreshRow = [=] {
            refreshEditor();
            const Param* prm = getParam();
            const bool anim = prm && prm->animated();
            sw->blockSignals(true);
            sw->setChecked(anim);
            sw->blockSignals(false);
            const Clip* c = s->seq().clip(cid);
            bool onKey = false;
            if (prm && c) onKey = prm->keyAt(s->playhead() - c->start, s->seq().frameDur() / 2) != nullptr;
            key->setIcon(icon(onKey ? "key" : "keyhollow", anim ? col::text : col::textDim));
            lane->update();
        };
        refreshers_.push_back(refreshRow);
        refreshRow();
        form->addWidget(row);
    };

    auto effectSection = [&](int effRef, const QString& title, bool removable) {
        const Effect* e = effectOf(*clip, effRef);
        const EffectDef* def = e ? findEffectDef(e->type) : nullptr;
        if (!def) return;
        QWidget* hdr = nullptr;
        QVBoxLayout* form = section(title, &hdr);
        auto* hl = static_cast<QHBoxLayout*>(hdr->layout());
        if (removable) {
            auto* en = new QCheckBox(hdr);
            en->setToolTip(tr("Effect on/off"));
            en->setChecked(e->enabled);
            hl->insertWidget(0, en);
            connect(en, &QCheckBox::toggled, hdr, [=](bool on) {
                s->edit(on ? tr("Enable Effect") : tr("Disable Effect"), [&](Project& p) {
                    if (Clip* c = p.sequence.clip(cid))
                        if (Effect* x = effectOf(*c, effRef)) x->enabled = on;
                });
            });
            auto* up = smallBtn("prev", tr("Move up"), hdr);
            up->setIcon(icon("prev"));
            auto* down = smallBtn("next", tr("Move down"), hdr);
            auto* del = smallBtn("trash", tr("Remove effect"), hdr);
            hl->addWidget(up);
            hl->addWidget(down);
            hl->addWidget(del);
            auto moveBy = [=](int d) {
                s->edit(tr("Reorder Effects"), [&](Project& p) {
                    Clip* c = p.sequence.clip(cid);
                    if (!c) return;
                    int j = effRef + d;
                    if (j < 0 || size_t(j) >= c->effects.size()) return;
                    std::swap(c->effects[size_t(effRef)], c->effects[size_t(j)]);
                });
            };
            connect(up, &QToolButton::clicked, hdr, [=] { moveBy(-1); });
            connect(down, &QToolButton::clicked, hdr, [=] { moveBy(1); });
            connect(del, &QToolButton::clicked, hdr, [=] {
                s->edit(tr("Remove Effect"), [&](Project& p) {
                    Clip* c = p.sequence.clip(cid);
                    if (c && effRef >= 0 && size_t(effRef) < c->effects.size()) c->effects.erase(c->effects.begin() + effRef);
                });
            });
        }
        auto* savePreset = smallBtn("preset", tr("Save as effect preset"), hdr);
        hl->addWidget(savePreset);
        connect(savePreset, &QToolButton::clicked, hdr, [=] {
            const Clip* c = s->seq().clip(cid);
            const Effect* x = c ? effectOf(*c, effRef) : nullptr;
            if (!x) return;
            bool ok = false;
            const QString n = QInputDialog::getText(hdr, tr("Save Effect Preset"), tr("Preset name:"), QLineEdit::Normal,
                                                    toQ(def->label), &ok)
                                  .trimmed();
            if (!ok || n.isEmpty()) return;
            Effect tmp;
            if (s->effectPreset(n, tmp) &&
                QMessageBox::question(hdr, tr("Save Effect Preset"), tr("Replace the preset \"%1\"?").arg(n)) != QMessageBox::Yes)
                return;
            s->saveEffectPreset(n, *x);
            emit s->message(tr("Saved preset \"%1\" (Effects panel > Presets)").arg(n));
        });
        auto* reset = smallBtn("reset", tr("Reset"), hdr);
        hl->addWidget(reset);
        connect(reset, &QToolButton::clicked, hdr, [=] {
            s->edit(tr("Reset Effect"), [&](Project& p) {
                if (Clip* c = p.sequence.clip(cid))
                    if (Effect* x = effectOf(*c, effRef)) {
                        Effect fresh = makeEffect(x->type);
                        fresh.path = x->path;
                        fresh.enabled = x->enabled;
                        *x = fresh;
                    }
            });
        });
        // position/anchor are shown in absolute frame / source pixels
        int srcW = 0, srcH = 0;
        SnapshotPtr snap = s->snapshot();
        clipSourceSize(*snap, *clip, srcW, srcH);
        const auto& st = s->seq().settings;
        for (const auto& pd : def->params) {
            double off = 0;
            if (e->type == fx::Motion) {
                if (pd.id == "posX") off = st.width / 2.0;
                if (pd.id == "posY") off = st.height / 2.0;
                if (pd.id == "anchorX") off = srcW / 2.0;
                if (pd.id == "anchorY") off = srcH / 2.0;
            }
            paramRow(form, effRef, pd, off);
        }
        if (e->type == fx::Lut) {
            auto* row = new QWidget(content_);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(26, 0, 0, 0);
            auto* path = new QLabel(e->path.empty() ? tr("(no LUT loaded)") : QFileInfo(toQ(e->path)).fileName(), row);
            path->setStyleSheet("color: #8c8c8c;");
            auto* browse = new QPushButton(tr("Load .cube..."), row);
            h->addWidget(path, 1);
            h->addWidget(browse);
            connect(browse, &QPushButton::clicked, row, [=] {
                QString f = QFileDialog::getOpenFileName(this, tr("Load LUT"), {}, tr("Cube LUT (*.cube)"));
                if (f.isEmpty()) return;
                s->edit(tr("Load LUT"), [&](Project& p) {
                    if (Clip* c = p.sequence.clip(cid))
                        if (Effect* x = effectOf(*c, effRef)) x->path = toS(QDir::toNativeSeparators(f));
                });
            });
            form->addWidget(row);
        }
    };

    // --- clip section -------------------------------------------------------------------------
    {
        QVBoxLayout* form = section(tr("Clip"));
        auto* row = new QWidget(content_);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        if (!audio) {
            h->addWidget(new QLabel(tr("Scale"), row));
            auto* fit = new QComboBox(row);
            fit->addItems({tr("Fit to frame"), tr("Fill frame"), tr("Native size")});
            fit->setCurrentIndex(int(clip->fit));
            connect(fit, &QComboBox::activated, row, [=](int i) {
                s->edit(tr("Scale Mode"), [&](Project& p) {
                    if (Clip* c = p.sequence.clip(cid)) c->fit = FitMode(i);
                });
            });
            h->addWidget(fit);
        }
        if (clip->kind == ClipKind::Media) {
            QString sp = clip->isHold() ? tr("Frame hold")
                                        : QString("%1%2%").arg(clip->reverse ? "-" : "").arg(clip->speed * 100, 0, 'g', 4);
            auto* speed = new QPushButton(tr("Speed: %1").arg(sp), row);
            connect(speed, &QPushButton::clicked, row, [s] { emit s->speedDialogRequested(); });
            h->addWidget(speed);
        }
        if (clip->kind == ClipKind::Matte) {
            auto* colorBtn = new QPushButton(tr("Color..."), row);
            const QColor cur = QColor::fromRgba(clip->matteColor);
            colorBtn->setIcon(swatchIcon(clip->matteColor));
            connect(colorBtn, &QPushButton::clicked, row, [=] {
                const QColor col = QColorDialog::getColor(cur, row, tr("Matte Color"), QColorDialog::ShowAlphaChannel);
                if (!col.isValid()) return;
                s->edit(tr("Matte Color"), [&](Project& p) {
                    if (Clip* c = p.sequence.clip(cid)) c->matteColor = col.rgba();
                });
            });
            h->addWidget(colorBtn);
        }
        h->addStretch(1);
        form->addWidget(row);
        if (audio || clip->kind == ClipKind::Text || clip->kind == ClipKind::Matte) {
            auto* fr = new QWidget(content_);
            auto* fh = new QHBoxLayout(fr);
            fh->setContentsMargins(0, 0, 0, 0);
            auto mkSpin = [&](const QString& label, Tick v, bool in) {
                fh->addWidget(new QLabel(label, fr));
                auto* sb = new QDoubleSpinBox(fr);
                sb->setRange(0, 600);
                sb->setDecimals(2);
                sb->setSuffix(" s");
                sb->setKeyboardTracking(false);
                sb->setValue(ticksToSeconds(v));
                connect(sb, &QDoubleSpinBox::valueChanged, fr, [=](double secs) {
                    s->edit(in ? tr("Fade In") : tr("Fade Out"), [&](Project& p) {
                        if (Clip* c = p.sequence.clip(cid))
                            (in ? c->fadeIn : c->fadeOut) = std::clamp<Tick>(s->snapToFrame(secondsToTicks(secs)), 0, c->duration);
                    });
                });
                fh->addWidget(sb);
            };
            mkSpin(tr("Fade in"), clip->fadeIn, true);
            mkSpin(tr("Fade out"), clip->fadeOut, false);
            fh->addStretch(1);
            form->addWidget(fr);
        }
        if (audio) {
            auto* gr = new QWidget(content_);
            auto* gh = new QHBoxLayout(gr);
            gh->setContentsMargins(0, 0, 0, 0);
            gh->addWidget(new QLabel(tr("Clip gain"), gr));
            auto* g = new QDoubleSpinBox(gr);
            g->setRange(-60, 24);
            g->setDecimals(1);
            g->setSuffix(" dB");
            g->setKeyboardTracking(false);
            g->setValue(clip->gainDb);
            connect(g, &QDoubleSpinBox::valueChanged, gr, [=](double v) {
                s->edit(tr("Clip Gain"), [&](Project& p) {
                    if (Clip* c = p.sequence.clip(cid)) c->gainDb = v;
                });
            });
            gh->addWidget(g);
            gh->addStretch(1);
            form->addWidget(gr);
        }
    }

    // --- title text -----------------------------------------------------------------------------
    if (clip->kind == ClipKind::Text) {
        QVBoxLayout* form = section(tr("Text"));
        auto* txt = new QPlainTextEdit(toQ(clip->text), content_);
        txt->setFixedHeight(64);
        txt->setToolTip(tr("Title text (applied when the box loses focus)"));
        form->addWidget(txt);
        auto commitText = [=] {
            QString t = txt->toPlainText();
            const Clip* c = s->seq().clip(cid);
            if (c && toQ(c->text) != t)
                s->edit(tr("Edit Title Text"), [&](Project& p) {
                    if (Clip* x = p.sequence.clip(cid)) x->text = toS(t);
                });
        };
        connect(qApp, &QApplication::focusChanged, txt, [=](QWidget* old, QWidget*) {
            if (old == txt) commitText();
        });
        auto styleEdit = [s, cid](const QString& label, const std::function<void(TextStyle&)>& f) {
            s->edit(label, [&](Project& p) {
                if (Clip* c = p.sequence.clip(cid)) f(c->textStyle);
            });
        };
        const TextStyle& st = clip->textStyle;
        // style presets: built-in looks + the user's own (QSettings "titlePresets")
        {
            auto* pr = new QHBoxLayout;
            auto* presets = new QComboBox(content_);
            presets->addItem(tr("Apply style preset..."));
            struct Builtin {
                QString name;
                TextStyle style;
            };
            std::vector<Builtin> builtins;
            {
                TextStyle t;  // clean white with soft shadow
                builtins.push_back({tr("Clean White"), t});
                t = TextStyle{};
                t.fill = 0xFFFFD43B;
                t.stroke = true;
                t.strokeWidth = 8;
                t.weight = 900;
                t.shadow = false;
                builtins.push_back({tr("Bold Yellow Outline"), t});
                t = TextStyle{};
                t.size = 64;
                t.weight = 600;
                t.align = 0;
                t.shadow = false;
                t.box = true;
                t.boxColor = 0xFF1E1E1E;
                t.boxOpacity = 75;
                t.boxPadding = 22;
                t.boxRadius = 18;
                builtins.push_back({tr("Lower Third Box"), t});
                t = TextStyle{};
                t.weight = 900;
                t.gradient = true;
                t.fill = 0xFFFFE070;
                t.fill2 = 0xFFFF5020;
                t.tracking = 8;
                t.shadowBlur = 14;
                t.shadowColor = 0xE0000000;
                builtins.push_back({tr("Gamer Gold"), t});
                t = TextStyle{};
                t.weight = 800;
                t.gradient = true;
                t.fill = 0xFF7DF9FF;
                t.fill2 = 0xFFB15CFF;
                t.stroke = true;
                t.strokeWidth = 4;
                t.strokeColor = 0xFF101020;
                builtins.push_back({tr("Neon"), t});
                builtins.push_back({tr("Subtitle"), defaultCaptionStyle().text});
            }
            for (const auto& b : builtins) presets->addItem(b.name);
            QSettings qs;
            qs.beginGroup("titlePresets");
            const QStringList user = qs.childKeys();
            qs.endGroup();
            if (!user.isEmpty()) presets->insertSeparator(presets->count());
            for (const QString& u : user) presets->addItem(u, u);
            connect(presets, &QComboBox::activated, content_, [=](int i) {
                if (i <= 0) return;
                TextStyle chosen;
                const QString key = presets->itemData(i).toString();
                if (key.isEmpty() && i - 1 < int(builtins.size())) chosen = builtins[size_t(i - 1)].style;
                else if (!textStyleFromJson(toS(QSettings().value("titlePresets/" + key).toString()), chosen)) return;
                styleEdit(tr("Apply Text Preset"), [&](TextStyle& t) {
                    const std::string font = t.font;  // keep the chosen font unless the preset sets one
                    t = chosen;
                    if (chosen.font.empty()) t.font = font;
                });
                presets->setCurrentIndex(0);
            });
            auto* savePreset = new QPushButton(tr("Save Style..."), content_);
            savePreset->setToolTip(tr("Save this title's look as a preset"));
            connect(savePreset, &QPushButton::clicked, content_, [=] {
                bool ok = false;
                QString n = QInputDialog::getText(this, tr("Save Style Preset"), tr("Preset name:"), QLineEdit::Normal, {}, &ok);
                if (!ok || n.trimmed().isEmpty()) return;
                const Clip* c = s->seq().clip(cid);
                if (!c) return;
                QSettings().setValue("titlePresets/" + n.trimmed(), toQ(textStyleToJson(c->textStyle)));
                emit s->message(tr("Saved style preset \"%1\"").arg(n.trimmed()), 2500);
            });
            pr->addWidget(presets, 1);
            pr->addWidget(savePreset);
            form->addLayout(pr);
        }
        auto* grid = new QFormLayout;
        grid->setContentsMargins(0, 4, 0, 0);
        auto* font = new QFontComboBox(content_);
        font->setCurrentFont(QFont(toQ(st.font)));
        connect(font, &QFontComboBox::currentFontChanged, content_, [=](const QFont& f) {
            styleEdit(tr("Font"), [&](TextStyle& t) { t.font = toS(f.family()); });
        });
        grid->addRow(tr("Font"), font);
        auto* sizeRow = new QHBoxLayout;
        auto* size = new QDoubleSpinBox(content_);
        size->setRange(4, 1000);
        size->setDecimals(0);
        size->setKeyboardTracking(false);
        size->setValue(st.size);
        connect(size, &QDoubleSpinBox::valueChanged, content_, [=](double v) {
            styleEdit(tr("Font Size"), [&](TextStyle& t) { t.size = v; });
        });
        auto* weight = new QComboBox(content_);
        weight->addItems({tr("Light"), tr("Regular"), tr("Medium"), tr("Semibold"), tr("Bold"), tr("Black")});
        const int weights[] = {300, 400, 500, 600, 700, 900};
        for (int i = 0; i < 6; ++i)
            if (weights[i] == st.weight) weight->setCurrentIndex(i);
        connect(weight, &QComboBox::activated, content_, [=](int i) {
            styleEdit(tr("Font Weight"), [&](TextStyle& t) { t.weight = weights[i]; });
        });
        auto* italic = new QCheckBox(tr("Italic"), content_);
        italic->setChecked(st.italic);
        connect(italic, &QCheckBox::toggled, content_, [=](bool on) {
            styleEdit(tr("Italic"), [&](TextStyle& t) { t.italic = on; });
        });
        auto* align = new QComboBox(content_);
        align->addItems({tr("Left"), tr("Center"), tr("Right")});
        align->setCurrentIndex(st.align);
        connect(align, &QComboBox::activated, content_, [=](int i) {
            styleEdit(tr("Alignment"), [&](TextStyle& t) { t.align = i; });
        });
        sizeRow->addWidget(size);
        sizeRow->addWidget(weight);
        sizeRow->addWidget(italic);
        sizeRow->addWidget(align);
        grid->addRow(tr("Size"), sizeRow);
        auto colorRow = [&](const QString& label, bool* enabled, uint32_t color, const std::function<void(TextStyle&, bool)>& setOn,
                            const std::function<void(TextStyle&, uint32_t)>& setColor, QWidget* extra = nullptr) {
            auto* h = new QHBoxLayout;
            QCheckBox* on = nullptr;
            if (enabled) {
                on = new QCheckBox(content_);
                on->setChecked(*enabled);
                connect(on, &QCheckBox::toggled, content_, [=](bool v) { styleEdit(label, [&](TextStyle& t) { setOn(t, v); }); });
                h->addWidget(on);
            }
            auto* cb = colorButton(color, content_);
            connect(cb, &QPushButton::clicked, content_, [=] {
                QColor c = QColorDialog::getColor(QColor::fromRgba(color), this, label, QColorDialog::ShowAlphaChannel);
                if (c.isValid()) styleEdit(label, [&](TextStyle& t) { setColor(t, c.rgba()); });
            });
            h->addWidget(cb);
            if (extra) h->addWidget(extra);
            h->addStretch(1);
            grid->addRow(label, h);
        };
        colorRow(tr("Fill"), nullptr, st.fill, nullptr, [](TextStyle& t, uint32_t c) { t.fill = c; });
        bool gradOn = st.gradient;
        colorRow(tr("Gradient to"), &gradOn, st.fill2, [](TextStyle& t, bool v) { t.gradient = v; },
                 [](TextStyle& t, uint32_t c) { t.fill2 = c; });
        auto* sw = new QDoubleSpinBox(content_);
        sw->setRange(0, 100);
        sw->setDecimals(1);
        sw->setPrefix(tr("width "));
        sw->setKeyboardTracking(false);
        sw->setValue(st.strokeWidth);
        connect(sw, &QDoubleSpinBox::valueChanged, content_, [=](double v) {
            styleEdit(tr("Stroke Width"), [&](TextStyle& t) { t.strokeWidth = v; });
        });
        bool strokeOn = st.stroke, shadowOn = st.shadow, boxOn = st.box;
        colorRow(tr("Stroke"), &strokeOn, st.strokeColor, [](TextStyle& t, bool v) { t.stroke = v; },
                 [](TextStyle& t, uint32_t c) { t.strokeColor = c; }, sw);
        auto* shadowBox = new QWidget(content_);
        auto* shl = new QHBoxLayout(shadowBox);
        shl->setContentsMargins(0, 0, 0, 0);
        for (auto [label, val, field] : {std::tuple{tr("x "), st.shadowX, 0}, {tr("y "), st.shadowY, 1}, {tr("blur "), st.shadowBlur, 2}}) {
            auto* sp = new QDoubleSpinBox(shadowBox);
            sp->setRange(field == 2 ? 0 : -200, 200);
            sp->setDecimals(0);
            sp->setPrefix(label);
            sp->setKeyboardTracking(false);
            sp->setValue(val);
            int fieldIdx = field;
            connect(sp, &QDoubleSpinBox::valueChanged, shadowBox, [=](double v) {
                styleEdit(tr("Shadow"), [&](TextStyle& t) { (fieldIdx == 0 ? t.shadowX : fieldIdx == 1 ? t.shadowY : t.shadowBlur) = v; });
            });
            shl->addWidget(sp);
        }
        colorRow(tr("Shadow"), &shadowOn, st.shadowColor, [](TextStyle& t, bool v) { t.shadow = v; },
                 [](TextStyle& t, uint32_t c) { t.shadowColor = c; }, shadowBox);
        auto* boxOpts = new QWidget(content_);
        auto* bol = new QHBoxLayout(boxOpts);
        bol->setContentsMargins(0, 0, 0, 0);
        auto* bop = new QDoubleSpinBox(boxOpts);
        bop->setRange(0, 100);
        bop->setDecimals(0);
        bop->setSuffix(" %");
        bop->setKeyboardTracking(false);
        bop->setValue(st.boxOpacity);
        connect(bop, &QDoubleSpinBox::valueChanged, boxOpts, [=](double v) {
            styleEdit(tr("Background Opacity"), [&](TextStyle& t) { t.boxOpacity = v; });
        });
        auto* bpad = new QDoubleSpinBox(boxOpts);
        bpad->setRange(0, 400);
        bpad->setDecimals(0);
        bpad->setPrefix(tr("pad "));
        bpad->setKeyboardTracking(false);
        bpad->setValue(st.boxPadding);
        connect(bpad, &QDoubleSpinBox::valueChanged, boxOpts, [=](double v) {
            styleEdit(tr("Background Padding"), [&](TextStyle& t) { t.boxPadding = v; });
        });
        auto* brad = new QDoubleSpinBox(boxOpts);
        brad->setRange(0, 400);
        brad->setDecimals(0);
        brad->setPrefix(tr("radius "));
        brad->setKeyboardTracking(false);
        brad->setValue(st.boxRadius);
        connect(brad, &QDoubleSpinBox::valueChanged, boxOpts, [=](double v) {
            styleEdit(tr("Background Radius"), [&](TextStyle& t) { t.boxRadius = v; });
        });
        bol->addWidget(bop);
        bol->addWidget(bpad);
        bol->addWidget(brad);
        colorRow(tr("Background"), &boxOn, st.boxColor, [](TextStyle& t, bool v) { t.box = v; },
                 [](TextStyle& t, uint32_t c) { t.boxColor = c; }, boxOpts);
        auto* ls = new QDoubleSpinBox(content_);
        ls->setRange(0.5, 4);
        ls->setSingleStep(0.05);
        ls->setDecimals(2);
        ls->setKeyboardTracking(false);
        ls->setValue(st.lineSpacing);
        connect(ls, &QDoubleSpinBox::valueChanged, content_, [=](double v) {
            styleEdit(tr("Line Spacing"), [&](TextStyle& t) { t.lineSpacing = v; });
        });
        ls->setMaximumWidth(110);
        grid->addRow(tr("Line spacing"), ls);
        auto* trk = new QDoubleSpinBox(content_);
        trk->setRange(-20, 100);
        trk->setDecimals(0);
        trk->setSuffix(" %");
        trk->setKeyboardTracking(false);
        trk->setValue(st.tracking);
        connect(trk, &QDoubleSpinBox::valueChanged, content_, [=](double v) {
            styleEdit(tr("Letter Spacing"), [&](TextStyle& t) { t.tracking = v; });
        });
        trk->setMaximumWidth(110);
        grid->addRow(tr("Letter spacing"), trk);
        form->addLayout(grid);
    }

    if (!audio) {
        effectSection(-1, tr("Motion"), false);
        effectSection(-2, tr("Opacity"), false);
    } else {
        effectSection(-3, tr("Volume"), false);
    }
    for (size_t i = 0; i < clip->effects.size(); ++i) {
        const EffectDef* d = findEffectDef(clip->effects[i].type);
        effectSection(int(i), d ? toQ(d->label) : toQ(clip->effects[i].type), true);
    }
    root->addStretch(1);
}

} // namespace cut::ui
