#pragma once
// The timeline: track headers, ruler, clips (thumbnails / waveforms), transitions, captions,
// markers, playhead; tools (selection, razor, ripple, hand, zoom), snapping, drag & drop.
// Paints only what is visible; layout is recomputed only when tracks/heights change.
#include "core/model.h"

#include <QWidget>

class QScrollBar;

namespace cut::ui {

class Session;

class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(Session* s, QWidget* parent = nullptr);
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void setPixelsPerSecond(double pps, double anchorX);
    double pixelsPerSecond() const { return pps_; }
    void ensureVisible(Tick t);
    // Widget coordinates of time t in the middle of a track's row (tests, tooltips).
    QPoint pointFor(Id track, Tick t) const;
    // Time under a widget x coordinate.
    Tick timeAtX(double x) const { return xToTime(x); }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    struct Row {
        Id track = 0;
        TrackKind kind = TrackKind::Video;
        int index = 0;  // within kind
        int y = 0, h = 0;
    };
    enum class Mode { None, Scrub, Pending, Move, TrimHead, TrimTail, Rubber, Pan, FadeIn, FadeOut, Gain,
                      TransitionResize, CaptionMove, CaptionHead, CaptionTail, DropAssets };
    struct Hit {
        enum Kind { Nothing, Clip, ClipHead, ClipTail, FadeIn, FadeOut, GainLine, Transition, TransitionEdge,
                    Caption, CaptionHead, CaptionTail } kind = Nothing;
        Id id = 0;
        int row = -1;
    };

    void layoutRows();
    int rowAt(int y) const;
    const Row* rowFor(Id track) const;
    double timeToX(Tick t) const;
    Tick xToTime(double x) const;
    int bodyLeft() const { return headerW_; }
    int bodyTop() const { return rulerH_; }
    Hit hitTest(QPointF p) const;
    QRectF clipRect(const Clip& c, const Row& r) const;
    Tick snap(Tick t, const std::vector<Id>& exclude, bool* snapped = nullptr);
    Tick snapDelta(Tick dt, const std::vector<Id>& moving, bool* snapped);
    std::vector<Tick> snapCandidates(const std::vector<Id>& exclude) const;
    void updateScrollBars();
    void zoomEdge(bool left, double fraction);  // zoom bar end dragged to fraction of the groove
    void updateCursor(QPointF p);
    int trackDeltaFor(const Row& from, int y) const;

    void paintRuler(QPainter& p);
    void paintHeaders(QPainter& p);
    void paintClip(QPainter& p, const Clip& c, const Row& r);
    void paintCaptions(QPainter& p, const Row& r);
    void paintTransitions(QPainter& p, const Row& r);
    void headerClick(const Row& r, QPointF pos, Qt::MouseButton b);
    void editTimecode();

    Session* s_;
    QScrollBar* hbar_;
    QScrollBar* vbar_;
    std::vector<Row> rows_;
    int contentH_ = 0;
    int headerW_ = 168, rulerH_ = 30;
    double pps_ = 40;      // pixels per second
    double scrollX_ = 0;   // pixels (time 0 = 0)
    int scrollY_ = 0;
    // interaction
    Mode mode_ = Mode::None;
    QPointF press_;
    Tick pressTime_ = 0;
    Hit pressHit_;
    std::vector<Id> moving_;
    Tick origEdge_ = 0;
    double origValue_ = 0;
    QRectF rubber_;
    bool snapShown_ = false;
    Tick snapAt_ = 0;
    double panStart_ = 0;
    // drop from project panel
    std::vector<Id> dropAssets_;
    struct DropSource {
        Tick in = 0, out = 0;
        bool video = true, audio = true, valid = false;
    } dropSource_;
    bool dropInsert_ = false;
    bool dropWasEmpty_ = false;
};

} // namespace cut::ui
