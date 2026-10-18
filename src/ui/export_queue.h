#pragma once
// Export queue: exports run one after another in the background (one GPU encoder at a time)
// while editing goes on. Each entry keeps the sequence as it was when it was queued.
#include "export/export_job.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <vector>

class QTreeWidget;
class QPushButton;

namespace cut::ui {

class Session;

class ExportQueue : public QObject {
    Q_OBJECT
public:
    enum class State { Waiting, Running, Done, Failed, Cancelled };
    struct Entry {
        int id = 0;
        QString label;  // sequence name
        SnapshotPtr snap;
        ExportSettings settings;
        State state = State::Waiting;
        double progress = 0;  // 0..1
        QString detail;       // progress / result text
        std::unique_ptr<ExportJob> job;
    };

    explicit ExportQueue(Session* s);
    ~ExportQueue() override;
    int add(SnapshotPtr snap, const ExportSettings& es, const QString& label);
    const std::vector<std::unique_ptr<Entry>>& entries() const { return entries_; }
    const Entry* entry(int id) const;
    int waitingBefore(int id) const;  // queued entries ahead of it
    void cancel(int id);              // waiting: dropped; running: stopped
    void remove(int id);              // a finished or waiting entry
    void clearFinished();
    void cancelAll();                 // and wait (application exit)
    bool busy() const;

signals:
    void changed();
    void entryFinished(int id);

private:
    void pump();
    Session* s_;
    std::vector<std::unique_ptr<Entry>> entries_;
    int nextId_ = 1;
    QTimer timer_;
};

QString exportStateName(ExportQueue::State s);

class ExportQueuePanel : public QWidget {
    Q_OBJECT
public:
    ExportQueuePanel(Session* s, QWidget* parent = nullptr);

private:
    void refresh();
    int currentId() const;
    Session* s_;
    QTreeWidget* list_;
    QPushButton *cancel_, *remove_, *clear_, *folder_;
};

} // namespace cut::ui
