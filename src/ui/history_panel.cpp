#include "ui/history_panel.h"

#include "core/time.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace cut::ui {

// ---------------------------------------------------------------------------------------------
// History

HistoryPanel::HistoryPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget(this);
    list_->setUniformItemSizes(true);
    list_->setToolTip(tr("Click a step to undo or redo to it"));
    v->addWidget(list_);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { jumpTo(size_t(list_->row(it))); });
    connect(s_, &Session::changed, this, &HistoryPanel::refresh);
    connect(s_, &Session::projectLoaded, this, [this] {
        size_ = pos_ = size_t(-1);
        refresh();
    });
    refresh();
}

void HistoryPanel::jumpTo(size_t pos) {
    const UndoStack& u = s_->doc().undo;
    pos = std::min(pos, u.size());
    jumping_ = true;
    while (u.position() > pos && u.canUndo()) s_->undo();
    while (u.position() < pos && u.canRedo()) s_->redo();
    jumping_ = false;
    refresh();
}

void HistoryPanel::refresh() {
    if (jumping_) return;
    const UndoStack& u = s_->doc().undo;
    if (u.size() == size_ && u.position() == pos_) return;
    // the labels of existing steps never change, so only append / trim the list
    if (size_ == size_t(-1) || list_->count() > int(u.size()) + 1) list_->clear();
    if (list_->count() == 0) list_->addItem(new QListWidgetItem(icon("film", col::textDim), tr("Open")));
    for (size_t i = size_t(list_->count()) - 1; i < u.size(); ++i)
        list_->addItem(new QListWidgetItem(icon("select", col::textDim), toQ(u.label(i))));
    // labels of steps after a new branch are replaced: refresh them all when the size shrank or
    // stayed equal while the position moved without undo/redo (rare; cheap enough)
    for (size_t i = 0; i < u.size(); ++i) list_->item(int(i) + 1)->setText(toQ(u.label(i)));
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        const bool undone = size_t(i) > u.position();
        it->setForeground(undone ? col::textDim.darker(130) : col::text);
        QFont f = it->font();
        f.setItalic(undone);
        it->setFont(f);
    }
    list_->setCurrentRow(int(u.position()));
    list_->scrollToItem(list_->currentItem());
    size_ = u.size();
    pos_ = u.position();
}

// ---------------------------------------------------------------------------------------------
// Markers

namespace {

class MarkerTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> onDelete;

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && state() != EditingState && onDelete) {
            onDelete();
            return;
        }
        QTreeWidget::keyPressEvent(e);
    }
};

} // namespace

MarkersPanel::MarkersPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* tree = new MarkerTree(this);
    tree->onDelete = [this] { removeSelected(); };
    tree_ = tree;
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Timecode"), tr("Name")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(true);
    tree_->setToolTip(tr("Double-click to jump to a marker; click a name to rename it"));
    v->addWidget(tree_);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int col) {
        if (col == 1) return;  // editing the name
        s_->setPlayhead(Tick(it->data(0, Qt::UserRole + 1).toLongLong()));
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
        if (filling_ || col != 1) return;
        const Id id = Id(it->data(0, Qt::UserRole).toULongLong());
        const std::string name = it->text(1).toStdString();
        s_->edit(tr("Rename Marker"), [&](Project& p) {
            for (auto& m : p.sequence.markers)
                if (m.id == id) m.name = name;
        });
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, &MarkersPanel::contextMenu);
    connect(s_, &Session::changed, this, &MarkersPanel::refresh);
    connect(s_, &Session::projectLoaded, this, &MarkersPanel::refresh);
    refresh();
}

void MarkersPanel::refresh() {
    const Sequence& sq = s_->seq();
    if (sq.markers == shown_ && sq.settings.fps == fps_ && tree_->topLevelItemCount() == int(shown_.size())) return;
    shown_ = sq.markers;
    fps_ = sq.settings.fps;
    std::vector<Marker> sorted = shown_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    std::vector<Id> keepSel;
    for (auto* it : tree_->selectedItems()) keepSel.push_back(Id(it->data(0, Qt::UserRole).toULongLong()));
    filling_ = true;
    tree_->clear();
    for (const Marker& m : sorted) {
        auto* it = new QTreeWidgetItem(tree_);
        it->setIcon(0, swatchIcon(m.color));
        it->setText(0, toQ(formatTimecode(m.t, fps_)));
        it->setText(1, m.name.empty() ? QString() : toQ(m.name));
        it->setData(0, Qt::UserRole, qulonglong(m.id));
        it->setData(0, Qt::UserRole + 1, qlonglong(m.t));
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        if (std::find(keepSel.begin(), keepSel.end(), m.id) != keepSel.end()) it->setSelected(true);
    }
    filling_ = false;
}

QString youtubeChapters(const Sequence& s) {
    std::vector<Marker> ms = s.markers;
    std::stable_sort(ms.begin(), ms.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    const bool hours = s.duration() >= 3600 * kTicksPerSecond;
    auto stamp = [&](Tick t) {
        const int64_t secs = int64_t(ticksToSeconds(t));
        const int h = int(secs / 3600), m = int(secs / 60 % 60), sec = int(secs % 60);
        return hours ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };
    QStringList lines;
    // YouTube needs the first chapter at 0:00
    if (ms.empty() || ms.front().t >= kTicksPerSecond) lines << stamp(0) + " " + QObject::tr("Intro");
    int n = 1;
    for (const Marker& m : ms) {
        const QString name = m.name.empty() ? QObject::tr("Chapter %1").arg(n) : toQ(m.name);
        lines << stamp(lines.isEmpty() ? 0 : m.t) + " " + name;
        ++n;
    }
    return lines.join('\n');
}

void MarkersPanel::copyChapters() {
    QGuiApplication::clipboard()->setText(youtubeChapters(s_->seq()));
    emit s_->message(tr("Chapters copied - paste them into the video description."));
}

void MarkersPanel::removeSelected() {
    std::vector<Id> ids;
    for (auto* it : tree_->selectedItems()) ids.push_back(Id(it->data(0, Qt::UserRole).toULongLong()));
    if (ids.empty()) return;
    s_->edit(ids.size() > 1 ? tr("Delete Markers") : tr("Delete Marker"), [&](Project& p) {
        auto& ms = p.sequence.markers;
        ms.erase(std::remove_if(ms.begin(), ms.end(),
                                [&](const Marker& m) { return std::find(ids.begin(), ids.end(), m.id) != ids.end(); }),
                 ms.end());
    });
}

void MarkersPanel::contextMenu(const QPoint& pos) {
    std::vector<Id> ids;
    for (auto* it : tree_->selectedItems()) ids.push_back(Id(it->data(0, Qt::UserRole).toULongLong()));
    QMenu menu(this);
    menu.addAction(tr("Add Marker at Playhead"), this, [this] { s_->addMarkerAtPlayhead(); });
    menu.addAction(tr("Copy as YouTube Chapters"), this, [this] { copyChapters(); })->setEnabled(!s_->seq().markers.empty());
    if (!ids.empty()) {
        if (auto* it = tree_->itemAt(pos)) {
            menu.addAction(tr("Go to Marker"), this, [this, it] {
                s_->setPlayhead(Tick(it->data(0, Qt::UserRole + 1).toLongLong()));
            });
            menu.addAction(tr("Rename"), this, [this, it] { tree_->editItem(it, 1); });
        }
        QMenu* colors = menu.addMenu(tr("Color"));
        for (const auto& c : kLabelColors) {
            const uint32_t argb = c.argb;
            colors->addAction(swatchIcon(argb), QCoreApplication::translate("Label", c.name), this, [this, ids, argb] {
                s_->edit(tr("Marker Color"), [&](Project& p) {
                    for (auto& m : p.sequence.markers)
                        if (std::find(ids.begin(), ids.end(), m.id) != ids.end()) m.color = argb;
                });
            });
        }
        menu.addSeparator();
        menu.addAction(tr("Delete"), QKeySequence::Delete, this, [this] { removeSelected(); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

} // namespace cut::ui
