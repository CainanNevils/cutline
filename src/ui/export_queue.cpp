#include "ui/export_queue.h"

#include "ui/session.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProcess>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace cut::ui {

QString exportStateName(ExportQueue::State s) {
    switch (s) {
    case ExportQueue::State::Waiting: return QObject::tr("Waiting");
    case ExportQueue::State::Running: return QObject::tr("Exporting");
    case ExportQueue::State::Done: return QObject::tr("Done");
    case ExportQueue::State::Failed: return QObject::tr("Failed");
    case ExportQueue::State::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

ExportQueue::ExportQueue(Session* s) : s_(s) {
    connect(&timer_, &QTimer::timeout, this, &ExportQueue::pump);
}

ExportQueue::~ExportQueue() { cancelAll(); }

int ExportQueue::add(SnapshotPtr snap, const ExportSettings& es, const QString& label) {
    auto e = std::make_unique<Entry>();
    e->id = nextId_++;
    e->label = label;
    e->snap = std::move(snap);
    e->settings = es;
    e->detail = tr("Waiting");
    const int id = e->id;
    entries_.push_back(std::move(e));
    emit changed();
    pump();  // starts it when nothing else runs
    return id;
}

const ExportQueue::Entry* ExportQueue::entry(int id) const {
    for (const auto& e : entries_)
        if (e->id == id) return e.get();
    return nullptr;
}

int ExportQueue::waitingBefore(int id) const {
    int n = 0;
    for (const auto& e : entries_) {
        if (e->id == id) return n;
        if (e->state == State::Waiting || e->state == State::Running) ++n;
    }
    return n;
}

bool ExportQueue::busy() const {
    for (const auto& e : entries_)
        if (e->state == State::Waiting || e->state == State::Running) return true;
    return false;
}

void ExportQueue::cancel(int id) {
    for (auto& e : entries_) {
        if (e->id != id) continue;
        if (e->state == State::Waiting) {
            e->state = State::Cancelled;
            e->detail = tr("Cancelled before it started.");
            emit changed();
            emit entryFinished(id);
        } else if (e->state == State::Running && e->job) {
            e->job->cancel();  // pump() sees it finish
        }
    }
}

void ExportQueue::remove(int id) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e->id == id; });
    if (it == entries_.end() || (*it)->state == State::Running) return;
    entries_.erase(it);
    emit changed();
}

void ExportQueue::clearFinished() {
    std::erase_if(entries_, [](const auto& e) { return e->state != State::Waiting && e->state != State::Running; });
    emit changed();
}

void ExportQueue::cancelAll() {
    timer_.stop();
    for (auto& e : entries_) {
        if (e->state == State::Waiting) e->state = State::Cancelled;
        if (e->job) {
            e->job->cancel();
            e->job->wait();
            e->job.reset();
            e->state = State::Cancelled;
        }
    }
}

void ExportQueue::pump() {
    Entry* run = nullptr;
    for (auto& e : entries_)
        if (e->state == State::Running) run = e.get();
    if (run && run->job) {
        ExportStatus& st = run->job->status();
        const int64_t done = st.framesDone.load(), total = std::max<int64_t>(1, st.framesTotal);
        const double an = st.analyzeProgress.load();
        QString enc;
        {
            std::lock_guard<std::mutex> l(st.m);
            enc = toQ(st.encoder);
        }
        if (an >= 0 && an < 1 && !st.finished.load()) {
            run->progress = an * 0.1;
            run->detail = tr("Measuring loudness... %1%").arg(int(an * 100));
        } else if (!st.finished.load()) {
            const double fps = st.fps.load();
            const Rational outFps = run->settings.fps.valid() ? run->settings.fps : run->snap->seq.settings.fps;
            const QString eta = fps > 0 ? toQ(formatDuration(secondsToTicks(double(total - done) / fps))) : QString("--");
            run->progress = double(done) / double(total);
            run->detail = tr("Frame %1 of %2   %3 fps (%4x realtime)   ETA %5\nEncoder: %6")
                              .arg(done)
                              .arg(total)
                              .arg(fps, 0, 'f', 0)
                              .arg(fps / outFps.toDouble(), 0, 'f', 2)
                              .arg(eta)
                              .arg(enc);
        } else {
            std::string err;
            {
                std::lock_guard<std::mutex> l(st.m);
                err = st.error;
            }
            const QString path = QDir::toNativeSeparators(toQ(run->settings.path));
            if (st.cancelled.load()) {
                run->state = State::Cancelled;
                run->detail = tr("Export cancelled.");
            } else if (st.failed.load()) {
                run->state = State::Failed;
                run->detail = tr("Export failed: %1").arg(toQ(err));
            } else {
                const double media = ticksToSeconds(run->settings.rangeEnd - run->settings.rangeStart);
                QString loud;
                if (run->settings.normalizeLufs != 0) {
                    std::lock_guard<std::mutex> l(st.m);
                    loud = tr("\nLoudness: measured %1 LUFS, gain %2%3 dB")
                               .arg(st.measuredLufs, 0, 'f', 1)
                               .arg(st.appliedGainDb >= 0 ? "+" : "")
                               .arg(st.appliedGainDb, 0, 'f', 1);
                }
                run->state = State::Done;
                run->progress = 1;
                run->detail = tr("Done: %1\n%2 frames in %3 s (%4x realtime). Encoder: %5")
                                  .arg(path)
                                  .arg(done)
                                  .arg(st.seconds, 0, 'f', 1)
                                  .arg(media / std::max(0.001, st.seconds), 0, 'f', 2)
                                  .arg(enc) +
                              loud;
            }
            run->job.reset();
            run->snap.reset();  // frees the sequence copy
            const int id = run->id;
            run = nullptr;
            emit entryFinished(id);
        }
    }
    if (!run) {
        for (auto& e : entries_)
            if (e->state == State::Waiting) {
                run = e.get();
                break;
            }
        if (run) {
            s_->stop();
            run->state = State::Running;
            run->detail = tr("Starting...");
            run->job = std::make_unique<ExportJob>(run->snap, run->settings);
            run->job->start();
        }
    }
    s_->setExporting(run != nullptr);
    if (run && !timer_.isActive()) timer_.start(200);
    if (!run) timer_.stop();
    emit changed();
}

// ---------------------------------------------------------------------------------------------
ExportQueuePanel::ExportQueuePanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    list_ = new QTreeWidget;
    list_->setHeaderLabels({tr("Output"), tr("Sequence"), tr("Status")});
    list_->setRootIsDecorated(false);
    list_->header()->setStretchLastSection(true);
    list_->setColumnWidth(0, 220);
    list_->setColumnWidth(1, 110);
    v->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    cancel_ = new QPushButton(tr("Cancel"));
    remove_ = new QPushButton(tr("Remove"));
    clear_ = new QPushButton(tr("Clear Finished"));
    folder_ = new QPushButton(tr("Show in Folder"));
    row->addWidget(cancel_);
    row->addWidget(remove_);
    row->addStretch(1);
    row->addWidget(folder_);
    row->addWidget(clear_);
    v->addLayout(row);
    ExportQueue* q = s_->exportQueue();
    connect(q, &ExportQueue::changed, this, &ExportQueuePanel::refresh);
    connect(list_, &QTreeWidget::currentItemChanged, this, &ExportQueuePanel::refresh);
    connect(cancel_, &QPushButton::clicked, this, [this] { s_->exportQueue()->cancel(currentId()); });
    connect(remove_, &QPushButton::clicked, this, [this] { s_->exportQueue()->remove(currentId()); });
    connect(clear_, &QPushButton::clicked, this, [this] { s_->exportQueue()->clearFinished(); });
    connect(folder_, &QPushButton::clicked, this, [this] {
        const ExportQueue::Entry* e = s_->exportQueue()->entry(currentId());
        if (!e) return;
        const QString path = QDir::toNativeSeparators(toQ(e->settings.path));
        if (QFileInfo::exists(path)) QProcess::startDetached("explorer.exe", {"/select,", path});
        else QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    refresh();
}

int ExportQueuePanel::currentId() const {
    const QTreeWidgetItem* it = list_->currentItem();
    return it ? it->data(0, Qt::UserRole).toInt() : 0;
}

void ExportQueuePanel::refresh() {
    const auto& entries = s_->exportQueue()->entries();
    // rows follow the queue; rebuild when entries were added or removed
    bool same = list_->topLevelItemCount() == int(entries.size());
    for (int i = 0; same && i < int(entries.size()); ++i)
        same = list_->topLevelItem(i)->data(0, Qt::UserRole).toInt() == entries[size_t(i)]->id;
    if (!same) {
        const int keep = currentId();
        QSignalBlocker block(list_);
        list_->clear();
        for (const auto& e : entries) {
            auto* it = new QTreeWidgetItem(list_);
            it->setData(0, Qt::UserRole, e->id);
            if (e->id == keep) list_->setCurrentItem(it);
        }
        if (!list_->currentItem() && list_->topLevelItemCount() > 0) list_->setCurrentItem(list_->topLevelItem(list_->topLevelItemCount() - 1));
    }
    for (int i = 0; i < int(entries.size()); ++i) {
        const auto& e = *entries[size_t(i)];
        QTreeWidgetItem* it = list_->topLevelItem(i);
        it->setText(0, QFileInfo(toQ(e.settings.path)).fileName());
        it->setToolTip(0, QDir::toNativeSeparators(toQ(e.settings.path)));
        it->setText(1, e.label);
        QString st = exportStateName(e.state);
        if (e.state == ExportQueue::State::Running) st += QString("  %1%").arg(int(e.progress * 100));
        it->setText(2, st);
        it->setToolTip(2, e.detail);
        const QColor c = e.state == ExportQueue::State::Done      ? QColor(0x6c, 0xc0, 0x6c)
                         : e.state == ExportQueue::State::Failed  ? QColor(0xe5, 0x48, 0x4d)
                         : e.state == ExportQueue::State::Running ? QColor(0x4d, 0xa3, 0xff)
                                                                   : QColor(0x9a, 0x9a, 0x9a);
        it->setForeground(2, c);
    }
    const ExportQueue::Entry* cur = s_->exportQueue()->entry(currentId());
    cancel_->setEnabled(cur && (cur->state == ExportQueue::State::Waiting || cur->state == ExportQueue::State::Running));
    remove_->setEnabled(cur && cur->state != ExportQueue::State::Running);
    folder_->setEnabled(cur != nullptr);
}

} // namespace cut::ui
