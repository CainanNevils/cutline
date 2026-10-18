#pragma once
// Source / Program monitor: video, mini scrub bar with in/out, timecode, transport, zoom,
// preview quality, proxy and safe-margin toggles, insert/overwrite (source), HUD (F3).
#include "core/model.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QTimer;

namespace cut::ui {

class Session;
class VideoWidget;

class ScrubBar : public QWidget {
    Q_OBJECT
public:
    ScrubBar(Session* s, bool program, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {200, 22}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    Tick duration() const;
    Tick position() const;
    void seekTo(int x);
    Session* s_;
    bool program_;
};

class MonitorPanel : public QWidget {
    Q_OBJECT
public:
    MonitorPanel(Session* s, bool program, QWidget* parent = nullptr);
    VideoWidget* video() const { return video_; }
    void setHud(bool on);
    bool hud() const { return hud_; }
    void markIn();
    void markOut();
    void step(int frames);
    void goIn();
    void goOut();

signals:
    void exportFrameRequested();

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void updateHint();
    void updateAudioImage();  // Source: waveform of an audio-only clip
    bool waveShown_ = false, waveComplete_ = false;
    void updateTimecode();
    void updateHud();
    void applyZoom();
    Tick position() const;
    Rational fps() const;
    Session* s_;
    bool program_;
    VideoWidget* video_;
    ScrubBar* scrub_;
    QLineEdit* tc_;
    QLabel* durLabel_;
    QToolButton* play_;
    QComboBox* zoom_;
    QComboBox* quality_ = nullptr;
    QToolButton* proxy_ = nullptr;
    QToolButton* safe_;
    QTimer* hudTimer_;
    bool hud_ = false;
};

} // namespace cut::ui
