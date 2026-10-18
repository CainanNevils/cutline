#pragma once
// History (the undo stack as a list: click a step to go back or forward to it) and Markers
// (sequence markers with timecode, editable name and colour; double-click jumps there).
#include "core/model.h"

#include <QWidget>

#include <vector>

class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace cut::ui {

class Session;

class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPanel(Session* s, QWidget* parent = nullptr);
    // Undo / redo until the stack position is `pos` (0 = the state before any edit).
    void jumpTo(size_t pos);
    QListWidget* list() const { return list_; }

private:
    void refresh();
    Session* s_;
    QListWidget* list_;
    size_t size_ = size_t(-1), pos_ = size_t(-1);
    bool jumping_ = false;
};

// Markers as YouTube chapter lines ("0:00 Intro", ...); the first chapter is always at 0:00.
QString youtubeChapters(const Sequence& s);

class MarkersPanel : public QWidget {
    Q_OBJECT
public:
    explicit MarkersPanel(Session* s, QWidget* parent = nullptr);
    QTreeWidget* tree() const { return tree_; }
    void copyChapters();

private:
    void refresh();
    void contextMenu(const QPoint& pos);
    void removeSelected();
    Session* s_;
    QTreeWidget* tree_;
    std::vector<Marker> shown_;
    Rational fps_;
    bool filling_ = false;
};

} // namespace cut::ui
