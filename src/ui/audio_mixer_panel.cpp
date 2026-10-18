#include "ui/audio_mixer_panel.h"

#include "ui/session.h"
#include "ui/theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace cut::ui {

namespace {
// fader position (0..1000) <-> dB, with -inf at the bottom
double posToDb(int p) {
    if (p <= 0) return -96;
    double f = p / 1000.0;
    return -60.0 + std::pow(f, 0.6) * 72.0;  // -60 .. +12
}
int dbToPos(double db) {
    if (db <= -60) return 0;
    double f = std::clamp((db + 60.0) / 72.0, 0.0, 1.0);
    return int(std::lround(std::pow(f, 1.0 / 0.6) * 1000));
}
double meterFrac(float lin) {
    double db = lin <= 1e-6f ? -96 : 20 * std::log10(lin);
    return std::clamp((db + 60.0) / 66.0, 0.0, 1.0);  // -60 .. +6 dB
}
} // namespace

PeakMeter::PeakMeter(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(18);
    setMinimumHeight(100);
    setToolTip(tr("Peak meter (click to reset the clip indicators)"));
}

void PeakMeter::push(float l, float r) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    float in[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
        level_[c] = std::max(in[c], level_[c] * 0.82f);  // ~20 dB/s fall at 30 Hz polling
        if (in[c] >= hold_[c] || now - holdAt_[c] > 1500) {
            hold_[c] = in[c];
            holdAt_[c] = now;
        }
        if (in[c] >= 1.0f) clip_[c] = true;
    }
    update();
}

void PeakMeter::mousePressEvent(QMouseEvent*) {
    clip_[0] = clip_[1] = false;
    update();
}

void PeakMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x12, 0x12, 0x12));
    const int w = (width() - 3) / 2;
    const int top = 6, h = height() - top - 2;
    for (int c = 0; c < 2; ++c) {
        int x = 1 + c * (w + 1);
        double f = meterFrac(level_[c]);
        QLinearGradient g(0, top + h, 0, top);
        g.setColorAt(0, QColor(0x2e, 0xa0, 0x4a));
        g.setColorAt(0.70, QColor(0x90, 0xc8, 0x30));
        g.setColorAt(0.85, QColor(0xe0, 0xc0, 0x30));
        g.setColorAt(0.91, QColor(0xe0, 0x40, 0x30));
        int bh = int(h * f);
        p.fillRect(QRect(x, top + h - bh, w, bh), g);
        double hf = meterFrac(hold_[c]);
        int hy = top + h - int(h * hf);
        p.fillRect(QRect(x, hy, w, 2), hf > 0.909 ? QColor(0xff, 0x50, 0x40) : QColor(0xe0, 0xe0, 0xe0));
        p.fillRect(QRect(x, 0, w, 4), clip_[c] ? QColor(0xff, 0x30, 0x30) : QColor(0x40, 0x20, 0x20));
    }
    // 0 dB and -6/-12/-24 ticks
    p.setPen(QColor(255, 255, 255, 60));
    for (double db : {0.0, -6.0, -12.0, -24.0, -48.0}) {
        int y = top + h - int(h * (db + 60.0) / 66.0);
        p.drawLine(0, y, width(), y);
    }
}

MeterScale::MeterScale(QWidget* parent) : QWidget(parent) {
    setFixedWidth(20);
    setMinimumHeight(100);
}

void MeterScale::paintEvent(QPaintEvent*) {
    QPainter p(this);
    QFont f = font();
    f.setPointSizeF(6.5);
    p.setFont(f);
    p.setPen(col::textDim);
    const int top = 6, h = height() - top - 2;  // same geometry as PeakMeter
    for (double db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
        const int y = top + h - int(h * (db + 60.0) / 66.0);
        p.drawText(QRect(0, y - 6, width() - 2, 12), Qt::AlignRight | Qt::AlignVCenter, QString::number(int(db)));
    }
}

AudioMixerPanel::AudioMixerPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    strips_ = new QHBoxLayout(inner);
    strips_->setContentsMargins(4, 4, 4, 4);
    strips_->setSpacing(4);
    scroll->setWidget(inner);
    outer->addWidget(scroll);
    connect(s_, &Session::structureChanged, this, &AudioMixerPanel::rebuild);
    connect(s_, &Session::changed, this, [this] {
        for (auto& f : refreshers_) f();
    });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &AudioMixerPanel::poll);
    timer_->start(33);
    rebuild();
}

void AudioMixerPanel::poll() {
    if (!s_->engine() || !isVisible()) return;
    MeterSnapshot m = s_->engine()->meters();
    for (auto& [id, pk] : m.tracks)
        if (auto it = meters_.find(id); it != meters_.end()) it->second->push(pk[0], pk[1]);
    if (master_) master_->push(m.master[0], m.master[1]);
    if (loudness_) {
        auto fmt = [](double v) { return v > -70 ? QString::number(v, 'f', 1) : QString("--"); };
        loudness_->setText(tr("%1 LUFS").arg(fmt(m.shortTerm)));
        const bool onTarget = m.shortTerm > -15.5 && m.shortTerm < -12.5;
        loudness_->setStyleSheet(onTarget ? "color: #3fb950; font-weight: 600;" : "color: #c8c8c8;");
        loudness_->setToolTip(tr("Short-term (3 s): %1 LUFS\nMomentary (400 ms): %2 LUFS\nIntegrated since Play: %3 LUFS\nYouTube targets -14 LUFS.")
                                  .arg(fmt(m.shortTerm), fmt(m.momentary), fmt(m.integrated)));
    }
}

void AudioMixerPanel::rebuild() {
    refreshers_.clear();
    meters_.clear();
    master_ = nullptr;
    loudness_ = nullptr;
    while (QLayoutItem* it = strips_->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    Session* s = s_;
    auto strip = [&](const QString& name, Id trackId, bool isMaster) {
        auto* w = new QWidget(this);
        w->setFixedWidth(isMaster ? 100 : 96);
        w->setStyleSheet("QWidget#strip { background: #262626; border-radius: 3px; }");
        w->setObjectName("strip");
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(3);
        auto* pan = new QSlider(Qt::Horizontal, w);
        pan->setRange(-100, 100);
        pan->setToolTip(tr("Pan (double-click to centre)"));
        if (isMaster) pan->setVisible(false);
        v->addWidget(pan);
        auto* btns = new QHBoxLayout;
        auto* mute = new QToolButton(w);
        mute->setText("M");
        mute->setCheckable(true);
        mute->setToolTip(tr("Mute"));
        auto* solo = new QToolButton(w);
        solo->setText("S");
        solo->setCheckable(true);
        solo->setToolTip(tr("Solo"));
        if (isMaster) {
            mute->setVisible(false);
            solo->setVisible(false);
        }
        btns->addWidget(mute);
        btns->addWidget(solo);
        v->addLayout(btns);
        QComboBox* duck = nullptr;
        QDoubleSpinBox* duckDb = nullptr;
        if (!isMaster) {
            duck = new QComboBox(w);
            duck->setToolTip(tr("Ducking: lower this track automatically while another track has sound\n"
                                "(e.g. the music under the microphone)"));
            duck->addItem(tr("No ducking"), qulonglong(0));
            for (const Track* other : s->seq().tracksOf(TrackKind::Audio))
                if (other->id != trackId) duck->addItem(tr("Duck under %1").arg(toQ(other->name)), qulonglong(other->id));
            duckDb = new QDoubleSpinBox(w);
            duckDb->setRange(-40, 0);
            duckDb->setDecimals(0);
            duckDb->setSuffix(" dB");
            duckDb->setKeyboardTracking(false);
            duckDb->setAlignment(Qt::AlignCenter);
            duckDb->setToolTip(tr("How far the track is lowered while ducked"));
            v->addWidget(duck);
            v->addWidget(duckDb);
            connect(duck, &QComboBox::activated, w, [=](int i) {
                const Id key = Id(duck->itemData(i).toULongLong());
                s->edit(tr("Ducking"), [&](Project& p) {
                    if (Track* t = p.sequence.track(trackId)) t->duckUnder = key;
                });
            });
            connect(duckDb, &QDoubleSpinBox::valueChanged, w, [=](double v) {
                s->edit(tr("Ducking Amount"), [&](Project& p) {
                    if (Track* t = p.sequence.track(trackId)) t->duckDb = v;
                });
            });
        }
        auto* fh = new QHBoxLayout;
        auto* fader = new QSlider(Qt::Vertical, w);
        fader->setRange(0, 1000);
        fader->setToolTip(tr("Volume (double-click the dB box to type)"));
        auto* meter = new PeakMeter(w);
        fh->addWidget(fader);
        fh->addWidget(new MeterScale(w));
        fh->addWidget(meter);
        fh->setSpacing(1);
        v->addLayout(fh, 1);
        auto* db = new QDoubleSpinBox(w);
        db->setRange(-96, 12);
        db->setDecimals(1);
        db->setSuffix(" dB");
        db->setKeyboardTracking(false);
        db->setButtonSymbols(QAbstractSpinBox::NoButtons);
        db->setAlignment(Qt::AlignCenter);
        v->addWidget(db);
        auto* label = new QLabel(name, w);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet("font-weight: 600;");
        v->addWidget(label);
        if (isMaster) {
            loudness_ = new QLabel(w);
            loudness_->setAlignment(Qt::AlignCenter);
            loudness_->setText(tr("-- LUFS"));
            loudness_->setToolTip(tr("Short-term loudness (last 3 s) of the mix while playing.\n"
                                     "Hover for momentary and integrated (since Play) values. YouTube targets -14 LUFS."));
            v->insertWidget(v->count() - 1, loudness_);
        }
        strips_->addWidget(w);
        if (isMaster) master_ = meter;
        else meters_[trackId] = meter;

        auto getDb = [s, trackId, isMaster]() -> double {
            if (isMaster) return s->seq().masterVolumeDb;
            const Track* t = s->seq().track(trackId);
            return t ? t->volumeDb : 0;
        };
        auto setDb = [trackId, isMaster](Project& p, double v) {
            if (isMaster) p.sequence.masterVolumeDb = v;
            else if (Track* t = p.sequence.track(trackId)) t->volumeDb = v;
        };
        connect(fader, &QSlider::sliderPressed, w, [=] { s->beginDrag(tr("Volume")); });
        connect(fader, &QSlider::sliderMoved, w, [=](int pos) {
            double v = posToDb(pos);
            s->updateDrag([&](Project& p) { setDb(p, v); });
            db->blockSignals(true);
            db->setValue(v);
            db->blockSignals(false);
        });
        connect(fader, &QSlider::sliderReleased, w, [=] { s->endDrag(true); });
        connect(db, &QDoubleSpinBox::valueChanged, w, [=](double v) { s->edit(tr("Volume"), [&](Project& p) { setDb(p, v); }); });
        if (!isMaster) {
            connect(pan, &QSlider::sliderPressed, w, [=] { s->beginDrag(tr("Pan")); });
            connect(pan, &QSlider::sliderMoved, w, [=](int v) {
                s->updateDrag([&](Project& p) {
                    if (Track* t = p.sequence.track(trackId)) t->pan = v / 100.0;
                });
            });
            connect(pan, &QSlider::sliderReleased, w, [=] { s->endDrag(true); });
            connect(mute, &QToolButton::toggled, w, [=](bool on) {
                s->edit(tr("Mute Track"), [&](Project& p) {
                    if (Track* t = p.sequence.track(trackId)) t->muted = on;
                });
            });
            connect(solo, &QToolButton::toggled, w, [=](bool on) {
                s->edit(tr("Solo Track"), [&](Project& p) {
                    if (Track* t = p.sequence.track(trackId)) t->solo = on;
                });
            });
        }
        auto refresh = [=] {
            if (!fader->isSliderDown()) {
                fader->blockSignals(true);
                fader->setValue(dbToPos(getDb()));
                fader->blockSignals(false);
            }
            if (!db->hasFocus()) {
                db->blockSignals(true);
                db->setValue(getDb());
                db->blockSignals(false);
            }
            if (!isMaster)
                if (const Track* t = s->seq().track(trackId)) {
                    if (!pan->isSliderDown()) {
                        pan->blockSignals(true);
                        pan->setValue(int(std::lround(t->pan * 100)));
                        pan->blockSignals(false);
                    }
                    mute->blockSignals(true);
                    mute->setChecked(t->muted);
                    mute->blockSignals(false);
                    solo->blockSignals(true);
                    solo->setChecked(t->solo);
                    solo->blockSignals(false);
                    duck->blockSignals(true);
                    duck->setCurrentIndex(std::max(0, duck->findData(qulonglong(t->duckUnder))));
                    duck->blockSignals(false);
                    duckDb->setVisible(t->duckUnder != 0);
                    if (!duckDb->hasFocus()) {
                        duckDb->blockSignals(true);
                        duckDb->setValue(t->duckDb);
                        duckDb->blockSignals(false);
                    }
                }
        };
        refreshers_.push_back(refresh);
        refresh();
    };
    for (const Track* t : s_->seq().tracksOf(TrackKind::Audio)) strip(toQ(t->name), t->id, false);
    strips_->addSpacing(10);
    strip(tr("Master"), 0, true);
    strips_->addStretch(1);
}

} // namespace cut::ui
