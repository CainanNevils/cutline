#pragma once
// Project panel: bins + assets (list with columns, or icon view with thumbnails), search,
// import (button, Ctrl+I, drag from Explorer), drag to the timeline, proxy status, relink.
#include "core/model.h"

#include <QPersistentModelIndex>
#include <QWidget>

#include <vector>

class QLabel;
class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItem;
class QTreeView;
class QModelIndex;

namespace cut::ui {

class Session;
class ProjectModel;

class ProjectPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProjectPanel(Session* s, QWidget* parent = nullptr);
    void importDialog();
    std::vector<Id> selectedIds() const;
    void removeSelected();  // asks before removing media used in the sequence

signals:
    void openProjectRequested(const QString& path);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;  // icon view hover scrubbing

private:
    void scrubIcon(const QModelIndex& idx, double frac);  // thumbnail at that fraction (< 0: poster)
    void updateEmptyHint();
    void rebuild();
    void refreshStatus();
    void contextMenu(const QPoint& pos);
    void activated(const QModelIndex& idx);
    Id currentBin() const;
    Session* s_;
    ProjectModel* model_;
    QSortFilterProxyModel* filter_;
    QTreeView* tree_;
    QListView* icons_;
    QStackedWidget* stack_;
    std::vector<QLabel*> emptyHints_;
    QLineEdit* search_;
    QPersistentModelIndex hover_;  // icon being scrubbed (proxy index)
    Id hoverAsset_ = 0;
    Tick hoverTime_ = -1;
};

} // namespace cut::ui
