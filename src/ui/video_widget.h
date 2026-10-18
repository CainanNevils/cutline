#pragma once
// Native child window that hosts a DXGI swapchain owned by the render thread. Qt never paints
// it. Program monitor: on-canvas transform handles for the selected clip and the Text tool.
#include "engine/playback.h"

#include <QPointF>
#include <QWidget>

namespace cut::ui {

class Session;

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    VideoWidget(Session* s, int viewer, bool program, QWidget* parent = nullptr);
    ~VideoWidget() override;
    QPaintEngine* paintEngine() const override { return nullptr; }
    void attach();
    void setView(const ViewSettings& v);
    ViewSettings view() const { return view_; }
    void refreshHandles();
    bool isProgram() const { return program_; }
    QPointF toWidget(QPointF s) const;  // sequence px -> widget (logical px)

signals:
    void zoomChanged();

protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent*) override {}
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    // widget (logical px) <-> sequence px
    double scale() const;  // sequence px -> logical px
    QPointF toSeq(QPointF p) const;
    Id hitClip(QPointF seqPt) const;
    bool cornersOf(Id clip, Corners& c) const;

    Session* s_;
    int viewer_;
    bool program_;
    bool attached_ = false;
    HWND hwnd_ = nullptr;
    ViewSettings view_;
    enum class Drag { None, Move, Scale, Rotate, Pan, MaskMove, MaskSize } drag_ = Drag::None;
    // Shape Mask of a clip as a quad in sequence pixels (corners TL, TR, BL, BR)
    bool maskQuad(const Clip& c, Corners& out) const;
    QPointF pressPos_;
    QPointF anchorScreen_;
    double startA_ = 0, startB_ = 0, startScale_ = 100, startRot_ = 0;
    Id dragClip_ = 0;
};

} // namespace cut::ui
