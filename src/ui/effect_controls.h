#pragma once
// Effect Controls: parameters of the selected clip. Every parameter can be keyframed
// (stopwatch, add/remove key at the playhead, previous/next key, and a mini keyframe lane).
// Values are scrubbable (drag left/right; Shift = x10, Ctrl = fine) or typed (click).
#include "core/model.h"

#include <QWidget>

#include <functional>

class QVBoxLayout;
class QScrollArea;
class QLabel;

namespace cut::ui {

class Session;

class ScrubValue : public QWidget {
    Q_OBJECT
public:
    explicit ScrubValue(QWidget* parent = nullptr);
    void setup(double min, double max, double step, int decimals, const QString& unit);
    void setValue(double v);
    double value() const { return v_; }
    QSize sizeHint() const override { return {80, 20}; }

signals:
    void dragStarted();
    void dragged(double v);
    void dragFinished();
    void edited(double v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    double v_ = 0, min_ = 0, max_ = 1, step_ = 1;
    int dec_ = 1;
    QString unit_;
    QPointF press_;
    double start_ = 0;
    bool dragging_ = false;
};

// Keyframes of one parameter across the clip.
class KeyLane : public QWidget {
    Q_OBJECT
public:
    KeyLane(Session* s, Id clip, std::function<const Param*()> get, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {120, height()}; }
    // Graph mode (double-click): the value curve, keyframes draggable in time and value.
    void setGraph(bool on);
    bool graph() const { return graph_; }
    QPointF keyPoint(Tick local, double v) const { return {xOf(local), graph_ ? yOf(v) : 10.0}; }  // (tests)
    bool animated() const {
        const Param* p = get_();
        return p && p->animated();
    }

signals:
    void keyMoveStarted(Tick from);
    // value is NaN when only the time changes
    void keyMoved(Tick from, Tick to, double value);
    void keyMoveFinished();
    void keyMenu(Tick at, const QPoint& global);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    double xOf(Tick local) const;
    double yOf(double v) const;
    double valueAt(double y) const;
    void fitRange();
    Tick localAt(double x) const;
    Session* s_;
    Id clip_;
    std::function<const Param*()> get_;
    Tick dragKey_ = kInvalidTick;
    bool dragging_ = false;
    bool graph_ = false;
    double vmin_ = 0, vmax_ = 1;  // value range shown (frozen while dragging)
    QPointF pressAt_;
};

class EffectControls : public QWidget {
    Q_OBJECT
public:
    explicit EffectControls(Session* s, QWidget* parent = nullptr);

private:
    void rebuild();
    void refresh();
    std::string signature() const;
    Session* s_;
    QScrollArea* scroll_;
    QWidget* content_ = nullptr;
    QLabel* header_;
    Id clip_ = 0;
    std::string sig_;
    std::vector<std::function<void()>> refreshers_;
};

} // namespace cut::ui
