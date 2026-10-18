#include "ui/project_panel.h"

#include "media/probe.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QMouseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace cut::ui {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kIsBinRole = Qt::UserRole + 2;
constexpr int kSeqRole = Qt::UserRole + 3;  // sequence items: Sequence::uid
constexpr int kThumbRole = Qt::UserRole + 4;  // icon view thumbnail set
enum Col { cName, cDuration, cResolution, cFps, cCodec, cAudio, cProxy, cCount };
} // namespace

class ProjectModel : public QStandardItemModel {
public:
    ProjectModel(Session* s, QObject* parent) : QStandardItemModel(parent), s_(s) {}
    QStringList mimeTypes() const override { return {"application/x-cutline-assets", "application/x-cutline-sequence", "text/uri-list"}; }
    QMimeData* mimeData(const QModelIndexList& idx) const override {
        QStringList ids;
        auto* md = new QMimeData;
        for (const auto& i : idx)
            if (i.column() == 0 && i.data(kSeqRole).toULongLong()) {
                md->setData("application/x-cutline-sequence", QByteArray::number(i.data(kSeqRole).toULongLong()));
                return md;  // a sequence drags on its own (to nest it)
            }
        for (const auto& i : idx)
            if (i.column() == 0 && !i.data(kIsBinRole).toBool()) ids << QString::number(i.data(kIdRole).toULongLong());
        md->setData("application/x-cutline-assets", ids.join(',').toUtf8());
        return md;
    }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction | Qt::MoveAction; }
    Qt::ItemFlags flags(const QModelIndex& i) const override {
        Qt::ItemFlags f = QStandardItemModel::flags(i);
        if (!i.isValid()) return f | Qt::ItemIsDropEnabled;
        if (i.data(kIsBinRole).toBool()) return f | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled;
        return (f | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled;
    }
    bool canDropMimeData(const QMimeData* md, Qt::DropAction, int, int, const QModelIndex&) const override {
        return md->hasUrls() || md->hasFormat("application/x-cutline-assets");
    }
    bool dropMimeData(const QMimeData* md, Qt::DropAction, int, int, const QModelIndex& parent) override {
        Id bin = 0;
        QModelIndex p = parent.sibling(parent.row(), 0);
        if (p.isValid()) bin = p.data(kIsBinRole).toBool() ? Id(p.data(kIdRole).toULongLong()) : 0;
        if (md->hasUrls()) {
            QStringList files;
            for (const QUrl& u : md->urls()) {
                QFileInfo fi(u.toLocalFile());
                if (fi.isFile() && isSupportedExtension(toS(fi.filePath()))) files << fi.filePath();
            }
            if (!files.isEmpty()) s_->importFiles(files, bin);
            return true;
        }
        std::vector<Id> ids;
        for (const QString& x : QString::fromUtf8(md->data("application/x-cutline-assets")).split(',', Qt::SkipEmptyParts))
            ids.push_back(Id(x.toULongLong()));
        s_->edit(QObject::tr("Move to Bin"), [&](Project& pr) {
            for (Id id : ids)
                if (Asset* a = pr.asset(id)) a->binId = bin;
        });
        return false;  // we rebuild ourselves
    }

private:
    Session* s_;
};

ProjectPanel::ProjectPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* top = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search"));
    search_->setClearButtonEnabled(true);
    top->addWidget(search_, 1);
    auto* listBtn = new QToolButton(this);
    listBtn->setIcon(icon("list"));
    listBtn->setToolTip(tr("List View"));
    listBtn->setCheckable(true);
    listBtn->setChecked(true);
    auto* gridBtn = new QToolButton(this);
    gridBtn->setIcon(icon("grid"));
    gridBtn->setToolTip(tr("Icon View"));
    gridBtn->setCheckable(true);
    auto* binBtn = new QToolButton(this);
    binBtn->setIcon(icon("folder"));
    binBtn->setToolTip(tr("New Bin"));
    auto* impBtn = new QToolButton(this);
    impBtn->setIcon(icon("import"));
    impBtn->setToolTip(tr("Import... (Ctrl+I)"));
    for (auto* b : {listBtn, gridBtn, binBtn, impBtn}) top->addWidget(b);
    v->addLayout(top);

    model_ = new ProjectModel(s, this);
    filter_ = new QSortFilterProxyModel(this);
    filter_->setSourceModel(model_);
    filter_->setRecursiveFilteringEnabled(true);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    filter_->setSortCaseSensitivity(Qt::CaseInsensitive);
    stack_ = new QStackedWidget(this);
    tree_ = new QTreeView(this);
    tree_->setModel(filter_);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::DragDrop);
    tree_->setSortingEnabled(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->header()->setStretchLastSection(false);
    icons_ = new QListView(this);
    icons_->setModel(filter_);
    icons_->setViewMode(QListView::IconMode);
    icons_->setIconSize({160, 90});
    icons_->setMouseTracking(true);
    icons_->viewport()->installEventFilter(this);
    icons_->setGridSize({176, 126});
    icons_->setResizeMode(QListView::Adjust);
    icons_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    icons_->setDragEnabled(true);
    icons_->setAcceptDrops(true);
    icons_->setDragDropMode(QAbstractItemView::DragDrop);
    icons_->setMovement(QListView::Static);
    icons_->setContextMenuPolicy(Qt::CustomContextMenu);
    icons_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    icons_->setWordWrap(true);
    stack_->addWidget(tree_);
    stack_->addWidget(icons_);
    for (QAbstractItemView* view : {static_cast<QAbstractItemView*>(tree_), static_cast<QAbstractItemView*>(icons_)}) {
        auto* hint = new QLabel(view->viewport());
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet("color: #8c8c8c;");
        hint->setTextFormat(Qt::RichText);
        connect(hint, &QLabel::linkActivated, this, [this](const QString& link) {
            if (link == "import") importDialog();
            else emit openProjectRequested(QUrl(link).toLocalFile());
        });
        auto* hl = new QVBoxLayout(view->viewport());
        hl->addWidget(hint);
        emptyHints_.push_back(hint);
    }
    v->addWidget(stack_, 1);

    connect(listBtn, &QToolButton::clicked, this, [=] {
        listBtn->setChecked(true);
        gridBtn->setChecked(false);
        stack_->setCurrentWidget(tree_);
    });
    connect(gridBtn, &QToolButton::clicked, this, [=] {
        gridBtn->setChecked(true);
        listBtn->setChecked(false);
        stack_->setCurrentWidget(icons_);
    });
    connect(binBtn, &QToolButton::clicked, this, [this] {
        bool ok = false;
        QString n = QInputDialog::getText(this, tr("New Bin"), tr("Name:"), QLineEdit::Normal, tr("Bin"), &ok);
        if (ok && !n.isEmpty()) s_->addBin(n, currentBin());
    });
    connect(impBtn, &QToolButton::clicked, this, &ProjectPanel::importDialog);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
        filter_->setFilterFixedString(t);
        if (!t.isEmpty()) tree_->expandAll();
    });
    connect(tree_, &QTreeView::doubleClicked, this, &ProjectPanel::activated);
    connect(icons_, &QListView::doubleClicked, this, &ProjectPanel::activated);
    connect(tree_, &QWidget::customContextMenuRequested, this, &ProjectPanel::contextMenu);
    connect(icons_, &QWidget::customContextMenuRequested, this, &ProjectPanel::contextMenu);
    connect(s_, &Session::structureChanged, this, &ProjectPanel::rebuild);
    connect(s_, &Session::mediaUpdated, this, &ProjectPanel::refreshStatus);
    connect(s_, &Session::recentChanged, this, &ProjectPanel::updateEmptyHint);
    rebuild();
}

void ProjectPanel::importDialog() {
    QSettings st;
    QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Media"), st.value("import/dir").toString(),
                                                      toQ(mediaFileFilter()));
    if (files.isEmpty()) return;
    st.setValue("import/dir", QFileInfo(files.first()).absolutePath());
    s_->importFiles(files, currentBin());
}

Id ProjectPanel::currentBin() const {
    QAbstractItemView* view = stack_->currentWidget() == tree_ ? static_cast<QAbstractItemView*>(tree_) : icons_;
    QModelIndex i = view->currentIndex();
    if (!i.isValid()) return 0;
    i = i.sibling(i.row(), 0);
    if (i.data(kIsBinRole).toBool()) return Id(i.data(kIdRole).toULongLong());
    return 0;
}

std::vector<Id> ProjectPanel::selectedIds() const {
    std::vector<Id> ids;
    QAbstractItemView* view = stack_->currentWidget() == tree_ ? static_cast<QAbstractItemView*>(tree_) : icons_;
    for (const auto& i : view->selectionModel()->selectedIndexes())
        if (i.column() == 0) ids.push_back(Id(i.data(kIdRole).toULongLong()));
    return ids;
}

void ProjectPanel::rebuild() {
    QSet<qulonglong> expanded;
    std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& parent) {
        for (int r = 0; r < filter_->rowCount(parent); ++r) {
            QModelIndex i = filter_->index(r, 0, parent);
            if (tree_->isExpanded(i)) expanded.insert(i.data(kIdRole).toULongLong());
            collect(i);
        }
    };
    collect({});
    const int nameWidth = tree_->property("nameSized").toBool() ? tree_->columnWidth(cName) : 230;  // clear() resets it
    model_->clear();
    model_->setHorizontalHeaderLabels({tr("Name"), tr("Duration"), tr("Resolution"), tr("Frame Rate"), tr("Codec"),
                                       tr("Audio"), tr("Proxy")});
    const Project& p = s_->project();
    std::map<Id, QStandardItem*> binItems;
    std::function<void(Id, QStandardItem*)> addBins = [&](Id parent, QStandardItem* into) {
        for (const auto& b : p.bins) {
            if (b.parent != parent) continue;
            auto* it = new QStandardItem(icon("folder", QColor(0xd8, 0xb0, 0x50)), toQ(b.name));
            it->setData(qulonglong(b.id), kIdRole);
            it->setData(true, kIsBinRole);
            QList<QStandardItem*> row{it};
            for (int c = 1; c < cCount; ++c) row << new QStandardItem();
            into->appendRow(row);
            binItems[b.id] = it;
            addBins(b.id, it);
        }
    };
    addBins(0, model_->invisibleRootItem());
    for (const auto& a : p.assets) {
        QStandardItem* parent = model_->invisibleRootItem();
        if (auto it = binItems.find(a.binId); it != binItems.end()) parent = it->second;
        const MediaInfo& m = a.info;
        QString ic = m.isImage ? "image" : m.hasVideo ? "film" : "wave";
        auto* name = new QStandardItem(icon(ic, a.offline ? col::offline : col::textDim), toQ(a.name));
        name->setData(qulonglong(a.id), kIdRole);
        name->setData(false, kIsBinRole);
        name->setToolTip(toQ(a.path) + (a.offline ? tr("\n(Media Offline)") : ""));
        if (a.offline) name->setForeground(col::offline);
        QList<QStandardItem*> row{name};
        row << new QStandardItem(m.isImage ? tr("Still") : toQ(formatTimecode(m.duration, m.hasVideo ? m.video.fps : Rational{30, 1})));
        row << new QStandardItem(m.hasVideo ? QString("%1 x %2").arg(m.video.width).arg(m.video.height) : QString());
        row << new QStandardItem(m.hasVideo && !m.isImage
                                     ? QString("%1%2").arg(m.video.fps.toDouble(), 0, 'f', 2).arg(m.video.vfr ? " VFR" : "")
                                     : QString());
        row << new QStandardItem(m.hasVideo ? toQ(m.video.codec) + (m.video.bitDepth > 8 ? " 10-bit" : "") +
                                                     (m.video.transfer != ColorTransfer::SDR ? " HDR" : "") : toQ(m.audio.empty() ? "" : m.audio[0].codec));
        QStringList au;
        for (const auto& s : m.audio) au << QString("%1ch %2k").arg(s.channels).arg(s.sampleRate / 1000.0, 0, 'g', 3);
        row << new QStandardItem(au.join(", "));
        row << new QStandardItem();
        parent->appendRow(row);
    }
    // sequences (double-click opens, drag onto a timeline nests)
    const auto uids = s_->sequenceUids();
    const bool listSeqs = !p.assets.empty() || !p.bins.empty() || uids.size() > 1;
    for (Id uid : listSeqs ? uids : std::vector<Id>{}) {
        const Sequence* sq = p.sequenceByUid(uid);
        if (!sq) continue;
        auto* name = new QStandardItem(icon("sequence", QColor(0x3c, 0xb0, 0xa0)), toQ(sq->name));
        name->setData(qulonglong(uid), kSeqRole);
        name->setData(false, kIsBinRole);
        name->setToolTip(tr("Sequence - double-click to open, drag onto a timeline to nest it"));
        QList<QStandardItem*> row{name};
        row << new QStandardItem(toQ(formatTimecode(sq->duration(), sq->settings.fps)));
        row << new QStandardItem(QString("%1 x %2").arg(sq->settings.width).arg(sq->settings.height));
        row << new QStandardItem(QString::number(sq->settings.fps.toDouble(), 'f', 2));
        for (int col = 4; col < cCount; ++col) row << new QStandardItem();
        model_->invisibleRootItem()->appendRow(row);
    }
    refreshStatus();
    // names get room first (a stretched column is squeezed to nothing by the others); the user can resize it
    tree_->header()->setSectionResizeMode(cName, QHeaderView::Interactive);
    tree_->setColumnWidth(cName, nameWidth);
    tree_->setProperty("nameSized", true);
    for (int c = 1; c < cCount; ++c) tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    std::function<void(const QModelIndex&)> restore = [&](const QModelIndex& parent) {
        for (int r = 0; r < filter_->rowCount(parent); ++r) {
            QModelIndex i = filter_->index(r, 0, parent);
            if (expanded.contains(i.data(kIdRole).toULongLong())) tree_->expand(i);
            restore(i);
        }
    };
    restore({});
    updateEmptyHint();
}

void ProjectPanel::updateEmptyHint() {
    const Project& p = s_->project();
    const bool empty = p.assets.empty() && p.bins.empty() && p.otherSequences.empty();
    QString html;
    if (empty) {
        html = tr("<a href='import' style='color:#4d9dff; text-decoration:none'>Import media</a> (Ctrl+I) or drop files here");
        QStringList recent;
        for (const QString& r : s_->recentProjects())
            if (QFileInfo::exists(r) && QFileInfo(r) != QFileInfo(s_->projectPath()) && recent.size() < 6)
                recent << QString("<a href='%1' style='color:#c8c8c8; text-decoration:none'>%2</a>"
                                  "&nbsp;&nbsp;<span style='color:#666'>%3</span>")
                              .arg(QUrl::fromLocalFile(r).toString(), QFileInfo(r).completeBaseName().toHtmlEscaped(),
                                   QDir::toNativeSeparators(QFileInfo(r).absolutePath()).toHtmlEscaped());
        if (!recent.isEmpty())
            html += "<br><br><br>" + tr("Recent projects") + "<br><br>" + recent.join("<br>");
    }
    for (QLabel* l : emptyHints_) {
        l->setText(html);
        l->setVisible(empty);
    }
}

void ProjectPanel::refreshStatus() {
    std::function<void(QStandardItem*)> walk = [&](QStandardItem* parent) {
        for (int r = 0; r < parent->rowCount(); ++r) {
            QStandardItem* it = parent->child(r, 0);
            if (it->data(kIsBinRole).toBool()) {
                walk(it);
                continue;
            }
            Id id = Id(it->data(kIdRole).toULongLong());
            QString st;
            if (const ProxyState* ps = s_->proxy(id)) {
                switch (ps->status) {
                case ProxyState::Queued: st = tr("Queued"); break;
                case ProxyState::Building:
                    st = ps->job ? tr("Building %1%").arg(int(ps->job->progress.load() * 100)) : tr("Building...");
                    break;
                case ProxyState::Ready: st = tr("Ready"); break;
                case ProxyState::Failed: st = tr("Failed"); break;
                default: break;
                }
            }
            if (QStandardItem* p = parent->child(r, cProxy); p && p->text() != st) p->setText(st);
            // thumbnail for the icon view
            if (it->data(kThumbRole).isNull())
                if (ThumbStripPtr t = s_->thumbs(id)) {
                    QImage img = t->strip.copy(t->w * std::min<int>(int(t->times.size()) - 1, 1), 0, t->w, t->h)
                                     .scaled(160, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    it->setIcon(QIcon(QPixmap::fromImage(img)));
                    it->setData(true, kThumbRole);
                }
        }
    };
    walk(model_->invisibleRootItem());
}

void ProjectPanel::removeSelected() {
    auto ids = selectedIds();
    if (ids.empty()) return;
    int used = 0;
    auto count = [&](const Sequence& sq) {
        for (Id id : ids)
            for (const auto& c : sq.clips)
                if (c.kind == ClipKind::Media && c.assetId == id) ++used;
    };
    count(s_->seq());
    for (const auto& o : s_->project().otherSequences) count(o);
    if (used && QMessageBox::question(this, tr("Remove"),
                                      tr("%1 clip(s) in the project's sequences use this media and will be removed. Continue?").arg(used)) !=
                    QMessageBox::Yes)
        return;
    s_->removeAssets(ids);
}

void ProjectPanel::activated(const QModelIndex& idx) {
    QModelIndex i = idx.sibling(idx.row(), 0);
    if (i.data(kIsBinRole).toBool()) return;
    if (const Id uid = i.data(kSeqRole).toULongLong()) {
        s_->switchSequence(s_->tabOfSequence(uid));
        return;
    }
    Id id = Id(i.data(kIdRole).toULongLong());
    const Asset* a = s_->project().asset(id);
    if (a && a->offline) {
        QString f = QFileDialog::getOpenFileName(this, tr("Relink %1").arg(toQ(a->name)), QFileInfo(toQ(a->path)).absolutePath(),
                                                 toQ(mediaFileFilter()));
        QString err;
        if (!f.isEmpty() && !s_->relinkAsset(id, f, err)) QMessageBox::warning(this, tr("Relink"), err);
        return;
    }
    s_->openInSource(id);
    if (id == hoverAsset_ && hoverTime_ >= 0) s_->setSourcePosition(hoverTime_);  // where the icon was scrubbed to
}

void ProjectPanel::scrubIcon(const QModelIndex& idx, double frac) {
    const QModelIndex src = filter_->mapToSource(idx.sibling(idx.row(), 0));
    QStandardItem* it = model_->itemFromIndex(src);
    const Id id = Id(src.data(kIdRole).toULongLong());
    ThumbStripPtr t = (it && !src.data(kIsBinRole).toBool() && !src.data(kSeqRole).toULongLong()) ? s_->thumbs(id) : nullptr;
    if (!t || t->times.empty()) return;
    const int n = int(t->times.size());
    const int k = frac < 0 ? std::min(n - 1, 1) : std::clamp(int(frac * n), 0, n - 1);
    const QImage img = t->strip.copy(t->w * k, 0, t->w, t->h).scaled(160, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    it->setIcon(QIcon(QPixmap::fromImage(img)));
    hoverAsset_ = frac < 0 ? 0 : id;
    hoverTime_ = frac < 0 ? -1 : t->times[size_t(k)];
}

bool ProjectPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == icons_->viewport()) {
        if (e->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent*>(e)->position().toPoint();
            const QModelIndex idx = icons_->indexAt(pos);
            if (hover_.isValid() && hover_ != idx) scrubIcon(hover_, -1);  // back to the poster frame
            hover_ = idx;
            if (idx.isValid()) {
                const QRect r = icons_->visualRect(idx);
                scrubIcon(idx, std::clamp(double(pos.x() - r.left()) / std::max(1, r.width()), 0.0, 0.999));
            }
        } else if (e->type() == QEvent::Leave) {
            if (hover_.isValid()) scrubIcon(hover_, -1);
            hover_ = QPersistentModelIndex();
        }
    }
    return QWidget::eventFilter(o, e);
}

void ProjectPanel::contextMenu(const QPoint& pos) {
    QAbstractItemView* view = qobject_cast<QAbstractItemView*>(sender());
    QModelIndex idx = view->indexAt(pos);
    QMenu m(this);
    if (const Id uid = idx.sibling(idx.row(), 0).data(kSeqRole).toULongLong()) {
        m.addAction(tr("Open Sequence"), this, [this, uid] { s_->switchSequence(s_->tabOfSequence(uid)); });
        m.addAction(tr("Nest into Open Sequence at Playhead"), this, [this, uid] {
            s_->placeSequence(uid, s_->playhead(), s_->targetVideo, s_->targetAudio, false);
        })->setEnabled(s_->canNest(uid));
        m.exec(view->viewport()->mapToGlobal(pos));
        return;
    }
    m.addAction(tr("Import..."), this, &ProjectPanel::importDialog);
    m.addAction(tr("New Bin"), this, [this] {
        bool ok = false;
        QString n = QInputDialog::getText(this, tr("New Bin"), tr("Name:"), QLineEdit::Normal, tr("Bin"), &ok);
        if (ok && !n.isEmpty()) s_->addBin(n, currentBin());
    });
    if (idx.isValid()) {
        QModelIndex i = idx.sibling(idx.row(), 0);
        Id id = Id(i.data(kIdRole).toULongLong());
        bool bin = i.data(kIsBinRole).toBool();
        m.addSeparator();
        m.addAction(tr("Rename"), this, [this, id, bin, i] {
            bool ok = false;
            QString n = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal, i.data().toString(), &ok);
            if (!ok || n.isEmpty()) return;
            s_->edit(tr("Rename"), [&](Project& p) {
                if (bin) {
                    for (auto& b : p.bins)
                        if (b.id == id) b.name = toS(n);
                } else if (Asset* a = p.asset(id)) a->name = toS(n);
            });
        });
        if (!bin) {
            const Asset* a = s_->project().asset(id);
            m.addAction(tr("Open in Source Monitor"), this, [this, id] { s_->openInSource(id); });
            if (a && a->info.hasVideo && !a->info.isImage)
                m.addAction(tr("Create Proxy"), this, [this, id] { s_->buildProxy(id); });
            m.addAction(tr("Relink Media..."), this, [this, id] {
                const Asset* x = s_->project().asset(id);
                if (!x) return;
                QString f = QFileDialog::getOpenFileName(this, tr("Relink %1").arg(toQ(x->name)),
                                                         QFileInfo(toQ(x->path)).absolutePath(), toQ(mediaFileFilter()));
                QString err;
                if (!f.isEmpty() && !s_->relinkAsset(id, f, err)) QMessageBox::warning(this, tr("Relink"), err);
            });
            if (a)
                m.addAction(tr("Reveal in Explorer"), this, [p = a->path] {
                    QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(toQ(p))});
                });
        }
        m.addSeparator();
        m.addAction(tr("Remove from Project"), this, &ProjectPanel::removeSelected);
    }
    m.exec(view->viewport()->mapToGlobal(pos));
}

} // namespace cut::ui
