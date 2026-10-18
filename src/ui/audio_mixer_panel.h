#pragma once
#include "core/model.h"

#include <QWidget>

#include <array>
#include <functional>
#include <map>
#include <vector>

class QHBoxLayout;
class QLabel;
class QTimer;

namespace cut::ui {

class Session;

// Stereo peak meter with 1.5 s peak hold and a latched clip indicator (click to reset).
class PeakMeter : public QWidget {
    Q_OBJECT
public:
    explicit PeakMeter(QWidget* parent = nullptr);
    void push(float l, float r);  // linear peaks since the last call
    QSize sizeHint() const override { return {22, 160}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    float level_[2] = {0, 0};  // displayed (decays)
    float hold_[2] = {0, 0};
    qint64 holdAt_[2] = {0, 0};
    bool clip_[2] = {false, false};
};

// dB labels matching PeakMeter's scale.
class MeterScale : public QWidget {
public:
    explicit MeterScale(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
};

class AudioMixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioMixerPanel(Session* s, QWidget* parent = nullptr);

private:
    void rebuild();
    void poll();
    Session* s_;
    QHBoxLayout* strips_;
    std::map<Id, PeakMeter*> meters_;
    PeakMeter* master_ = nullptr;
    QLabel* loudness_ = nullptr;  // master short-term LUFS
    QTimer* timer_;
    std::vector<std::function<void()>> refreshers_;
};

} // namespace cut::ui
