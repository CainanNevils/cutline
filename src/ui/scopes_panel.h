#pragma once
// Video scopes for the Program monitor: luma waveform, RGB parade, vectorscope and histogram.
// The engine reads back a small copy of each composited frame (async, ~15 Hz while playing)
// only while this panel is visible; the scope images are built here on the UI thread.
#include <QImage>
#include <QWidget>

#include <array>

class QComboBox;

namespace cut::ui {

class Session;

class ScopesPanel : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Waveform, Parade, Vectorscope, Histogram };
    explicit ScopesPanel(Session* s, QWidget* parent = nullptr);
    Mode mode() const { return mode_; }
    void setMode(Mode m);
    // Builds the scope from an RGBA8 frame (public for tests).
    void setFrame(const QImage& rgba);
    const QImage& scopeImage() const { return scope_; }
    // Read-back runs only while active (the dock tab is the visible one).
    void setActive(bool on);
    bool active() const { return active_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    void rebuild();
    QRect plotRect() const;
    Session* s_;
    QComboBox* modeBox_;
    Mode mode_ = Mode::Waveform;
    QImage frame_;   // last frame (RGBA8888)
    QImage scope_;   // scope raster, scaled into the plot rect when painted
    std::array<std::array<int, 256>, 4> hist_{};  // R, G, B, Y
    bool active_ = false;
};

} // namespace cut::ui
