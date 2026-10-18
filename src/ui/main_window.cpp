#include "ui/main_window.h"

#include "core/edits.h"
#include "export/gif_export.h"
#include "ui/audio_mixer_panel.h"
#include "ui/history_panel.h"
#include "ui/scopes_panel.h"
#include "ui/captions_panel.h"
#include "ui/dialogs.h"
#include "ui/effect_controls.h"
#include "ui/effects_panel.h"
#include "ui/export_queue.h"
#include "ui/monitor_panel.h"
#include "ui/project_panel.h"
#include "ui/session.h"
#include "ui/shortcuts_dialog.h"
#include "ui/theme.h"
#include "ui/timeline_widget.h"
#include "ui/video_widget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QRegularExpression>
#include <QDir>
#include <QTabBar>
#include <QToolButton>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QMenu>
#include <QProgressDialog>
#include <QThread>
#include <QPointer>
#include <QTextBrowser>
#include <QTextCursor>
#include <QFile>
#include <QTimer>

#include <cmath>
#include <QToolBar>
#include <QVBoxLayout>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace cut::ui {

MainWindow::MainWindow(Session* s) : s_(s) {
    setWindowIcon(icon("cutline"));
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    buildDocks();
    buildMenus();
    statusDecode_ = new QLabel(this);
    statusDecode_->setTextFormat(Qt::RichText);
    statusJobs_ = new QLabel(this);
    statusRec_ = new QLabel(this);
    statusRec_->setVisible(false);
    statusBar()->addPermanentWidget(statusRec_);
    statusBar()->addPermanentWidget(statusJobs_);
    {
        auto* recTimer = new QTimer(this);
        connect(recTimer, &QTimer::timeout, this, [this] {
            const double sec = s_->voiceoverSeconds();
            const float pk = s_->voiceoverPeak();
            const double db = pk > 1e-5f ? 20.0 * std::log10(double(pk)) : -99.0;
            const int bars = std::clamp(int((db + 60.0) / 6.0), 0, 10);
            statusRec_->setText(QString("<span style='color:#ff4040'>&#9679; REC</span> %1:%2 "
                                        "<span style='color:%3'>%4</span><span style='color:#555'>%5</span>")
                                    .arg(int(sec) / 60).arg(int(sec) % 60, 2, 10, QChar('0'))
                                    .arg(db > -3 ? "#ff5050" : "#4cd964")
                                    .arg(QString(bars, QChar(0x2588))).arg(QString(10 - bars, QChar(0x2588))));
        });
        connect(s_, &Session::voiceoverChanged, this, [this, recTimer](bool on) {
            statusRec_->setVisible(on);
            if (recordAct_) recordAct_->setChecked(on);
            if (on) recTimer->start(100);
            else recTimer->stop();
        });
    }
    statusBar()->addPermanentWidget(statusDecode_);
    connect(s_, &Session::message, this, [this](const QString& t, int ms) { statusBar()->showMessage(t, ms); });
    connect(s_, &Session::titleChanged, this, &MainWindow::updateTitle);
    connect(s_, &Session::sourceChanged, this, [this] {
        const Asset* a = s_->project().asset(s_->sourceAsset());
        docks_["source"]->setWindowTitle(a ? tr("Source: %1").arg(toQ(a->name)) : tr("Source"));
    });
    connect(s_, &Session::hdrImported, this, [this](const QString& names) {
        auto* box = new QMessageBox(QMessageBox::Warning, tr("HDR media"),
                                    tr("%1 is HDR (PQ/HLG). Cutline shows and exports it as SDR with a simple tone map; HDR grading is not supported.").arg(names),
                                    QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    });
    connect(s_, &Session::gainDialogRequested, this, [this] {
        bool any = false;
        double cur = 0;
        for (Id id : s_->selection())
            if (const Clip* c = s_->seq().clip(id); c && s_->seq().track(c->trackId)->kind == TrackKind::Audio) {
                any = true;
                cur = c->gainDb;
            }
        if (!any) {
            statusBar()->showMessage(tr("Select audio clips first."), 3000);
            return;
        }
        bool ok = false;
        double v = QInputDialog::getDouble(this, tr("Audio Gain"), tr("Set gain to (dB):"), cur, -60, 24, 1, &ok);
        if (ok) s_->setSelectedGain(v, false);
    });
    connect(s_, &Session::sequenceMismatch, this, [this](Id asset) {
        const Asset* a = s_->project().asset(asset);
        if (!a) return;
        const auto& v = a->info.video;
        auto r = QMessageBox::question(
            this, tr("Clip Mismatch"),
            tr("This clip (%1 x %2, %3 fps) does not match the sequence (%4 x %5, %6 fps).\nChange the sequence to match the clip?")
                .arg(v.width).arg(v.height).arg(v.fps.toDouble(), 0, 'g', 5)
                .arg(s_->seq().settings.width).arg(s_->seq().settings.height).arg(s_->seq().settings.fps.toDouble(), 0, 'g', 5),
            QMessageBox::Yes | QMessageBox::No);
        if (r != QMessageBox::Yes) return;
        SequenceSettings ns = s_->seq().settings;
        ns.width = v.width & ~1;
        ns.height = v.height & ~1;
        if (v.fps.valid()) ns.fps = v.fps;
        s_->edit(tr("Match Sequence to Clip"), [&](Project& p) {
            const Rational oldFps = p.sequence.settings.fps;
            p.sequence.settings = ns;
            p.sequence.settings.fps = oldFps;
            if (!(ns.fps == oldFps)) edit::changeFrameRate(p.sequence, p.assets, ns.fps);
        });
    });
    connect(s_, &Session::speedDialogRequested, this, [this] {
        if (!s_->primaryClip()) return;
        SpeedDialog d(s_, this);
        if (d.exec() == QDialog::Accepted) d.apply();
    });
    connect(s_, &Session::engineError, this, [this](const QString& e) {
        statusBar()->showMessage(e, 10000);
        QString err;
        if (!s_->restartEngine(err)) QMessageBox::critical(this, tr("GPU"), tr("Could not restart the preview: %1").arg(err));
    });
    auto* statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, [this] {
        if (!s_->engine()) return;
        ViewerStats st = s_->engine()->stats(s_->programViewer());
        AudioStatus au = s_->engine()->audioStatus();
        QString dec = st.swSources ? tr("<span style='color:#e0a040'>CPU decode fallback</span>") : tr("GPU decode");
        statusDecode_->setText(QString("%1&nbsp;&nbsp;|&nbsp;&nbsp;%2 %3").arg(dec, toQ(au.backend), au.ok ? "" : tr("(no audio device)")));
        int jobs = s_->jobs().running() + s_->jobs().pending();
        statusJobs_->setText(jobs ? tr("Background: %1 job(s)%2").arg(jobs).arg(s_->jobs().paused() ? tr(" (paused)") : "") : QString());
    });
    statusTimer->start(500);
    // which monitor do transport keys address? the last one that had focus
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        for (QWidget* w = now; w; w = w->parentWidget()) {
            if (w == source_) {
                sourceActive_ = true;
                return;
            }
            if (w == program_ || w == timeline_) {
                sourceActive_ = false;
                return;
            }
        }
    });
    updateTitle();
    QSettings st;
    restoreGeometry(st.value("window/geometry").toByteArray());
    // a saved layout from before panels were added would leave the new panels unplaced
    constexpr int kLayoutVersion = 3;  // bump when docks are added
    if (st.value("window/layoutVersion").toInt() != kLayoutVersion || !restoreState(st.value("window/state").toByteArray())) {
        resetWorkspace();
        QTimer::singleShot(0, this, [this] { resetWorkspace(); });  // again once the window has its size
    }
}

void MainWindow::buildDocks() {
    source_ = new MonitorPanel(s_, false, this);
    program_ = new MonitorPanel(s_, true, this);
    timeline_ = new TimelineWidget(s_, this);
    project_ = new ProjectPanel(s_, this);
    effects_ = new EffectsPanel(s_, this);
    fxControls_ = new EffectControls(s_, this);
    mixer_ = new AudioMixerPanel(s_, this);
    captions_ = new CaptionsPanel(s_, this);
    scopes_ = new ScopesPanel(s_, this);
    history_ = new HistoryPanel(s_, this);
    markers_ = new MarkersPanel(s_, this);
    auto dock = [&](const QString& name, const QString& title, QWidget* w) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(name);
        d->setWidget(w);
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        docks_[name] = d;
        return d;
    };
    dock("source", tr("Source"), source_);
    dock("fxcontrols", tr("Effect Controls"), fxControls_);
    dock("mixer", tr("Audio Mixer"), mixer_);
    dock("program", tr("Program"), program_);
    dock("project", tr("Project"), project_);
    dock("effects", tr("Effects"), effects_);
    dock("captions", tr("Captions"), captions_);
    dock("scopes", tr("Scopes"), scopes_);
    // tabbed docks stay "visible" behind other tabs; this signal tracks the tab actually shown
    connect(docks_["scopes"], &QDockWidget::visibilityChanged, scopes_, &ScopesPanel::setActive);
    dock("markers", tr("Markers"), markers_);
    dock("history", tr("History"), history_);
    dock("queue", tr("Export Queue"), new ExportQueuePanel(s_, this));
    connect(s_->exportQueue(), &ExportQueue::changed, this, [this] {
        // queued in the background: bring the queue forward once
        static int shown = 0;
        const auto& es = s_->exportQueue()->entries();
        const int last = es.empty() ? 0 : es.back()->id;
        if (last != shown && !es.empty() && es.back()->state == ExportQueue::State::Waiting && s_->exportQueue()->waitingBefore(last) > 0) {
            shown = last;
            docks_["queue"]->show();
            docks_["queue"]->raise();
        }
    });
    // sequence tabs above the timeline
    auto* tlBox = new QWidget(this);
    auto* tlv = new QVBoxLayout(tlBox);
    tlv->setContentsMargins(0, 0, 0, 0);
    tlv->setSpacing(0);
    auto* tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(4, 2, 4, 0);
    seqTabs_ = new QTabBar(tlBox);
    seqTabs_->setObjectName("sequenceTabs");
    seqTabs_->setDocumentMode(true);
    seqTabs_->setExpanding(false);
    seqTabs_->setDrawBase(false);
    seqTabs_->setFixedHeight(26);  // (the row layout otherwise leaves it at zero height)
    seqTabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    seqTabs_->setToolTip(tr("Sequences - double-click to rename, right-click for more"));
    auto* addSeq = new QToolButton(tlBox);
    addSeq->setIcon(icon("plus"));
    addSeq->setAutoRaise(true);
    addSeq->setToolTip(tr("New Sequence"));
    tabRow->addWidget(seqTabs_);
    tabRow->addWidget(addSeq);
    tabRow->addStretch(1);
    tlv->addLayout(tabRow);
    tlv->addWidget(timeline_, 1);
    dock("timeline", tr("Timeline"), tlBox);
    connect(addSeq, &QToolButton::clicked, this, &MainWindow::newSequenceDialog);
    connect(seqTabs_, &QTabBar::tabBarClicked, this, [this](int i) {
        if (i >= 0 && i != s_->activeSequence()) s_->switchSequence(i);
    });
    connect(seqTabs_, &QTabBar::tabBarDoubleClicked, this, [this](int i) { renameSequenceDialog(i); });
    connect(seqTabs_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int i = seqTabs_->tabAt(pos);
        if (i < 0) return;
        QMenu m(this);
        m.addAction(tr("Rename..."), this, [this, i] { renameSequenceDialog(i); });
        m.addAction(tr("Duplicate"), this, [this, i] {
            s_->switchSequence(i);
            s_->duplicateSequence();
        });
        m.addSeparator();
        m.addAction(tr("Delete"), this, [this, i] {
            if (QMessageBox::question(this, tr("Delete Sequence"), tr("Delete the sequence \"%1\"? (Undo brings it back.)")
                                                                         .arg(s_->sequenceNames().value(i))) == QMessageBox::Yes)
                s_->deleteSequence(i);
        })->setEnabled(seqTabs_->count() > 1);
        m.exec(seqTabs_->mapToGlobal(pos));
    });
    auto refreshTabs = [this] {
        const QStringList names = s_->sequenceNames();
        QStringList shown;
        for (int i = 0; i < seqTabs_->count(); ++i) shown << seqTabs_->tabText(i);
        if (shown != names) {
            QSignalBlocker b(seqTabs_);
            while (seqTabs_->count()) seqTabs_->removeTab(0);
            for (const QString& n : names) seqTabs_->addTab(n);
        }
        QSignalBlocker b(seqTabs_);
        seqTabs_->setCurrentIndex(s_->activeSequence());
    };
    connect(s_, &Session::changed, this, refreshTabs);
    auto programTitle = [this] { docks_["program"]->setWindowTitle(tr("Program: %1").arg(toQ(s_->seq().name))); };
    connect(s_, &Session::changed, this, programTitle);
    connect(s_, &Session::projectLoaded, this, programTitle);
    connect(s_, &Session::projectLoaded, this, refreshTabs);
    refreshTabs();
    // tools strip
    auto* toolsW = new QWidget(this);
    auto* tv = new QVBoxLayout(toolsW);
    tv->setContentsMargins(2, 4, 2, 4);
    tv->setSpacing(2);
    auto* bar = new QToolBar(toolsW);
    bar->setOrientation(Qt::Vertical);
    bar->setIconSize({20, 20});
    tools_ = new QActionGroup(this);
    struct T {
        const char* icon;
        QString tip;
        QKeySequence key;
        Session::Tool tool;
    };
    const T tools[] = {{"select", tr("Selection Tool (V)"), QKeySequence("V"), Session::Tool::Select},
                       {"razor", tr("Razor Tool (C)"), QKeySequence("C"), Session::Tool::Razor},
                       {"ripple", tr("Ripple Edit Tool (B)"), QKeySequence("B"), Session::Tool::Ripple},
                       {"text", tr("Type Tool (T)"), QKeySequence("T"), Session::Tool::Text},
                       {"hand", tr("Hand Tool (H)"), QKeySequence("H"), Session::Tool::Hand},
                       {"zoom", tr("Zoom Tool (Z)"), QKeySequence("Z"), Session::Tool::Zoom},
                       {"ratestretch", tr("Rate Stretch Tool (R) - drag a clip's end to change its speed"), QKeySequence("R"),
                        Session::Tool::RateStretch}};
    for (const T& t : tools) {
        QAction* a = bar->addAction(icon(t.icon), t.tip);
        a->setObjectName(QString("cmd.tool.") + t.icon);
        a->setProperty("defaultShortcut", t.key);
        a->setCheckable(true);
        a->setShortcut(t.key);
        a->setShortcutContext(Qt::WindowShortcut);
        tools_->addAction(a);
        Session::Tool tool = t.tool;
        connect(a, &QAction::triggered, this, [this, tool] { s_->setTool(tool); });
        if (tool == Session::Tool::Select) a->setChecked(true);
        addAction(a);
    }
    connect(s_, &Session::toolChanged, this, [this] {
        tools_->actions().at(int(s_->tool()))->setChecked(true);
    });
    bar->addSeparator();
    snapAct_ = bar->addAction(icon("magnet"), tr("Snap (S)"));
    snapAct_->setCheckable(true);
    snapAct_->setChecked(true);
    connect(snapAct_, &QAction::toggled, this, [this](bool on) {
        s_->snapping = on;
        statusBar()->showMessage(on ? tr("Snapping on") : tr("Snapping off"), 1500);
    });
    auto* linkAct = bar->addAction(icon("link"), tr("Linked Selection"));
    linkAct->setCheckable(true);
    linkAct->setChecked(true);
    connect(linkAct, &QAction::toggled, this, [this](bool on) { s_->linkedSelection = on; });
    tv->addWidget(bar);
    tv->addStretch(1);
    dock("tools", tr("Tools"), toolsW);
    docks_["tools"]->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    docks_["tools"]->setTitleBarWidget(new QWidget(docks_["tools"]));  // a narrow strip: no "..." title
}

void MainWindow::resetWorkspace() {
    for (auto& [n, d] : docks_) {
        removeDockWidget(d);
        d->setFloating(false);
    }
    addDockWidget(Qt::TopDockWidgetArea, docks_["source"]);
    splitDockWidget(docks_["source"], docks_["program"], Qt::Horizontal);
    tabifyDockWidget(docks_["source"], docks_["fxcontrols"]);
    tabifyDockWidget(docks_["fxcontrols"], docks_["mixer"]);
    tabifyDockWidget(docks_["mixer"], docks_["scopes"]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_["project"]);
    splitDockWidget(docks_["project"], docks_["tools"], Qt::Horizontal);
    splitDockWidget(docks_["tools"], docks_["timeline"], Qt::Horizontal);
    tabifyDockWidget(docks_["project"], docks_["effects"]);
    tabifyDockWidget(docks_["effects"], docks_["captions"]);
    tabifyDockWidget(docks_["captions"], docks_["markers"]);
    tabifyDockWidget(docks_["markers"], docks_["history"]);
    tabifyDockWidget(docks_["history"], docks_["queue"]);
    for (auto& [n, d] : docks_) d->show();
    docks_["source"]->raise();
    docks_["project"]->raise();
    resizeDocks({docks_["source"], docks_["program"]}, {width() * 2 / 5, width() * 3 / 5}, Qt::Horizontal);
    resizeDocks({docks_["project"], docks_["tools"], docks_["timeline"]}, {width() * 3 / 10, 40, width() * 7 / 10},
                Qt::Horizontal);
    resizeDocks({docks_["source"], docks_["project"]}, {height() / 2, height() / 2}, Qt::Vertical);
}

QAction* MainWindow::act(QMenu* m, const QString& text, const QKeySequence& key, std::function<void()> fn,
                         const QString& iconName) {
    QAction* a = m->addAction(text);
    QString id = text;
    id.remove('&').remove(QRegularExpression("[^A-Za-z0-9]+"));
    a->setObjectName("cmd." + id);
    a->setProperty("defaultShortcut", key);
    if (!iconName.isEmpty()) a->setIcon(icon(iconName));
    if (!key.isEmpty()) {
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
    }
    connect(a, &QAction::triggered, this, std::move(fn));
    return a;
}

MonitorPanel* MainWindow::activeMonitor() const { return sourceActive_ ? source_ : program_; }
int MainWindow::activeViewer() const { return sourceActive_ ? s_->sourceViewer() : s_->programViewer(); }

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    act(file, tr("New Project..."), QKeySequence("Ctrl+Alt+N"), [this] { newProject(); });
    act(file, tr("Open Project..."), QKeySequence::Open, [this] { openProject(); });
    QMenu* recent = file->addMenu(tr("Open Recent"));
    auto fillRecent = [this, recent] {
        recent->clear();
        const QStringList l = s_->recentProjects();
        for (const QString& p : l)
            recent->addAction(QFileInfo(p).completeBaseName() + "   " + QDir::toNativeSeparators(QFileInfo(p).absolutePath()), this,
                              [this, p] { openProjectPath(p); });
        recent->setEnabled(!l.isEmpty());
    };
    connect(recent, &QMenu::aboutToShow, this, fillRecent);
    fillRecent();
    act(file, tr("Save"), QKeySequence::Save, [this] { save(false); });
    act(file, tr("Save As..."), QKeySequence("Ctrl+Shift+S"), [this] { save(true); });
    act(file, tr("Export GIF..."), {}, [this] {
        const Sequence& sq = s_->seq();
        Tick from = 0, to = sq.duration();
        if (sq.inPoint != kInvalidTick && sq.outPoint != kInvalidTick && sq.outPoint > sq.inPoint) {
            from = sq.inPoint;
            to = sq.outPoint;
        }
        if (to - from > kGifMaxLength) {
            QMessageBox::information(this, tr("Export GIF"), tr("GIFs are for short moments (up to 60 s). Mark In and Out around it first (I / O)."));
            return;
        }
        QDialog d(this);
        d.setWindowTitle(tr("Export GIF"));
        auto* form = new QFormLayout(&d);
        auto* width = new QComboBox(&d);
        width->addItems({"320", "480", "640", "800"});
        width->setCurrentIndex(1);
        auto* fps = new QComboBox(&d);
        fps->addItems({"10", "15", "20", "25"});
        fps->setCurrentIndex(1);
        form->addRow(tr("Width (px)"), width);
        form->addRow(tr("Frame rate"), fps);
        form->addRow(new QLabel(tr("%1 of video (%2)").arg(toQ(formatDuration(to - from)),
                                                             sq.inPoint != kInvalidTick ? tr("In to Out") : tr("whole sequence")), &d));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        form->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        QSettings st;
        const QString path = QFileDialog::getSaveFileName(this, tr("Export GIF"),
                                                          st.value("export/dir").toString() + "/" + s_->displayName() + ".gif",
                                                          tr("GIF (*.gif)"));
        if (path.isEmpty()) return;
        GifSettings gs;
        gs.path = toS(QDir::toNativeSeparators(path));
        gs.width = width->currentText().toInt();
        gs.fps = fps->currentText().toInt();
        gs.from = from;
        gs.to = to;
        auto progress = std::make_shared<std::atomic<double>>(0.0);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        auto result = std::make_shared<std::pair<bool, std::string>>();
        SnapshotPtr snap = s_->snapshot(true);
        s_->stop();
        std::thread worker([=] {
            std::string err;
            result->first = exportGif(snap, gs, *progress, *cancel, err);
            result->second = err;
            *finished = true;
        });
        QProgressDialog pd(tr("Rendering GIF..."), tr("Cancel"), 0, 1000, this);
        pd.setWindowModality(Qt::WindowModal);
        pd.setMinimumDuration(0);
        connect(&pd, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
        while (!finished->load()) {
            pd.setValue(int(progress->load() * 1000));
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.join();
        pd.close();
        if (result->first) statusBar()->showMessage(tr("GIF written: %1").arg(path), 8000);
        else if (!cancel->load()) QMessageBox::warning(this, tr("Export GIF"), toQ(result->second));
    });
    act(file, tr("Collect Files..."), {}, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Collect Files: choose an empty folder"));
        if (dir.isEmpty()) return;
        const bool onlyUsed = QMessageBox::question(this, tr("Collect Files"),
                                                    tr("Copy only the media used in the sequences?\n(No copies every imported file.)")) ==
                              QMessageBox::Yes;
        auto prog = std::make_shared<Session::CollectProgress>();
        auto result = std::make_shared<std::pair<bool, QString>>();
        auto finished = std::make_shared<std::atomic<bool>>(false);
        Project snapshot = s_->project();
        const QString name = s_->displayName();
        std::thread worker([prog, result, finished, snapshot, name, dir, onlyUsed]() mutable {
            QString err;
            result->first = Session::collectFiles(std::move(snapshot), name, dir, onlyUsed, *prog, err);
            result->second = err;
            *finished = true;  // (result is read only after this)
        });
        QProgressDialog pd(tr("Copying media..."), tr("Cancel"), 0, 1000, this);
        pd.setWindowModality(Qt::WindowModal);
        pd.setMinimumDuration(0);
        connect(&pd, &QProgressDialog::canceled, this, [prog] { prog->cancel = true; });
        QTimer poll;
        connect(&poll, &QTimer::timeout, &pd, [&] {
            const uint64_t tot = prog->total.load();
            pd.setValue(tot ? int(prog->done.load() * 1000 / tot) : 0);
            pd.setLabelText(tr("Copying media... %1 of %2 MB").arg(prog->done.load() >> 20).arg(tot >> 20));
        });
        poll.start(100);
        while (!finished->load()) {  // modal progress; the worker copies
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.join();
        poll.stop();
        pd.close();
        if (result->first)
            QMessageBox::information(this, tr("Collect Files"),
                                     tr("Saved %1 with its media.\nOpen that project to keep working from the new folder.")
                                         .arg(QDir::toNativeSeparators(prog->projectPath)));
        else if (!prog->cancel.load())
            QMessageBox::warning(this, tr("Collect Files"), result->second);
    });
    file->addSeparator();
    act(file, tr("Import..."), QKeySequence("Ctrl+I"), [this] { project_->importDialog(); }, "import");
    act(file, tr("Remove Unused Media"), {}, [this] {
        const auto ids = s_->unusedAssets();
        if (ids.empty()) {
            statusBar()->showMessage(tr("Every imported clip is used in a sequence."), 4000);
            return;
        }
        if (QMessageBox::question(this, tr("Remove Unused Media"),
                                  tr("Remove %n clip(s) that no sequence uses from the project? The files stay on disk; Undo brings "
                                     "them back.", "", int(ids.size()))) != QMessageBox::Yes)
            return;
        s_->removeAssets(ids);
    });
    act(file, tr("Export Media..."), QKeySequence("Ctrl+M"), [this] {
        if (s_->seq().duration() <= 0) {
            QMessageBox::information(this, tr("Export"), tr("The sequence is empty."));
            return;
        }
        auto* d = new ExportDialog(s_, this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
    });
    QAction* exportFrameAct = act(file, tr("Export Frame..."), QKeySequence("Ctrl+Shift+E"), [this] {
        QSettings st;
        QString f = QFileDialog::getSaveFileName(this, tr("Export Frame"),
                                                 st.value("export/dir").toString() + "/" + s_->displayName() + "_" +
                                                     toQ(formatTimecode(s_->playhead(), s_->seq().settings.fps)).replace(':', '-') + ".png",
                                                 tr("PNG (*.png);;JPEG (*.jpg)"));
        if (f.isEmpty()) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString err;
        bool ok = s_->exportFrame(f, err);
        QApplication::restoreOverrideCursor();
        if (!ok) QMessageBox::warning(this, tr("Export Frame"), err);
    });
    connect(program_, &MonitorPanel::exportFrameRequested, exportFrameAct, &QAction::trigger);
    act(file, tr("Export Thumbnail (1280x720 JPG)..."), {}, [this] {
        QSettings st;
        QString f = QFileDialog::getSaveFileName(this, tr("Export Thumbnail"),
                                                 st.value("export/dir").toString() + "/" + s_->displayName() + "_thumbnail.jpg",
                                                 tr("JPEG (*.jpg)"));
        if (f.isEmpty()) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString err;
        const bool ok = s_->exportThumbnail(f, err);
        QApplication::restoreOverrideCursor();
        if (!ok) QMessageBox::warning(this, tr("Export Thumbnail"), err);
    });
    connect(project_, &ProjectPanel::openProjectRequested, this, &MainWindow::openProjectPath);
    file->addSeparator();
    act(file, tr("New Sequence..."), QKeySequence("Ctrl+N"), [this] { newSequenceDialog(); });
    act(file, tr("Duplicate Sequence"), {}, [this] { s_->duplicateSequence(); });
    act(file, tr("Sequence Settings..."), {}, [this] {
        SequenceSettingsDialog d(s_->seq().settings, this);
        if (d.exec() != QDialog::Accepted) return;
        SequenceSettings ns = d.settings();
        s_->edit(tr("Sequence Settings"), [&](Project& p) {
            const Rational oldFps = p.sequence.settings.fps;
            p.sequence.settings = ns;
            p.sequence.settings.fps = oldFps;
            if (!(ns.fps == oldFps)) edit::changeFrameRate(p.sequence, p.assets, ns.fps);
        });
    });
    act(file, tr("Preferences..."), {}, [this] {
        PreferencesDialog d(s_, this);
        if (d.exec() == QDialog::Accepted) d.apply();
    });
    file->addSeparator();
    act(file, tr("Exit"), QKeySequence("Ctrl+Q"), [this] { close(); });

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = act(edit, tr("Undo"), QKeySequence::Undo, [this] { s_->undo(); });
    QAction* redo = act(edit, tr("Redo"), QKeySequence("Ctrl+Shift+Z"), [this] { s_->redo(); });
    connect(edit, &QMenu::aboutToShow, this, [this, undo, redo] {
        const auto& u = s_->doc().undo;
        undo->setText(u.canUndo() ? tr("Undo %1").arg(toQ(u.undoLabel())) : tr("Undo"));
        redo->setText(u.canRedo() ? tr("Redo %1").arg(toQ(u.redoLabel())) : tr("Redo"));
        undo->setEnabled(u.canUndo());
        redo->setEnabled(u.canRedo());
    });
    edit->addSeparator();
    act(edit, tr("Cut"), QKeySequence::Cut, [this] { s_->cutSelection(); });
    act(edit, tr("Copy"), QKeySequence::Copy, [this] { s_->copySelection(); });
    act(edit, tr("Paste"), QKeySequence::Paste, [this] { s_->paste(false); });
    act(edit, tr("Paste Insert"), QKeySequence("Ctrl+Shift+V"), [this] { s_->paste(true); });
    act(edit, tr("Paste Attributes..."), QKeySequence("Ctrl+Alt+V"), [this] {
        if (!s_->hasClipboardClip() || s_->selection().empty()) {
            s_->pasteAttributes(false, false, false, false);  // explains what to do
            return;
        }
        QDialog d(this);
        d.setWindowTitle(tr("Paste Attributes"));
        auto* v = new QVBoxLayout(&d);
        QSettings st;
        auto box = [&](const QString& text, const char* key) {
            auto* b = new QCheckBox(text, &d);
            b->setChecked(st.value(QString("pasteAttr/") + key, true).toBool());
            v->addWidget(b);
            return b;
        };
        auto* motion = box(tr("Motion (position, scale, rotation, crop keyframes)"), "motion");
        auto* opacity = box(tr("Opacity and blend mode"), "opacity");
        auto* effects = box(tr("Video effects"), "effects");
        auto* volume = box(tr("Volume, gain and fades"), "volume");
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        v->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        st.setValue("pasteAttr/motion", motion->isChecked());
        st.setValue("pasteAttr/opacity", opacity->isChecked());
        st.setValue("pasteAttr/effects", effects->isChecked());
        st.setValue("pasteAttr/volume", volume->isChecked());
        s_->pasteAttributes(motion->isChecked(), opacity->isChecked(), effects->isChecked(), volume->isChecked());
    });
    act(edit, tr("Clear"), QKeySequence(Qt::Key_Delete), [this] {
        if (QWidget* fw = QApplication::focusWidget(); fw && project_->isAncestorOf(fw)) {
            project_->removeSelected();  // Delete in the Project panel removes media
            return;
        }
        s_->deleteSelection(false);
    });
    act(edit, tr("Ripple Delete"), QKeySequence("Shift+Del"), [this] { s_->deleteSelection(true); });
    edit->addSeparator();
    act(edit, tr("Select All"), QKeySequence::SelectAll, [this] { s_->selectAll(); });
    act(edit, tr("Deselect All"), QKeySequence("Ctrl+Shift+A"), [this] { s_->setSelection({}); });
    edit->addSeparator();
    act(edit, tr("Keyboard Shortcuts..."), QKeySequence("Ctrl+Alt+K"), [this] {
        ShortcutsDialog d(findChildren<QAction*>(), this);
        if (d.exec() == QDialog::Accepted) d.apply();
    });

    QMenu* clip = menuBar()->addMenu(tr("&Clip"));
    act(clip, tr("Speed/Duration..."), QKeySequence("Ctrl+R"), [this] { emit s_->speedDialogRequested(); });
    act(clip, tr("Enable"), QKeySequence("Shift+E"), [this] { s_->toggleEnable(); });
    act(clip, tr("Link / Unlink"), QKeySequence("Ctrl+L"), [this] { s_->toggleLink(); });
    act(clip, tr("Nest"), QKeySequence("Ctrl+Alt+G"), [this] { s_->nestSelection(); });
    act(clip, tr("Replace With Source Clip"), {}, [this] { s_->replaceWithSourceClip(); });
    act(clip, tr("Track Mask Forward"), QKeySequence("Ctrl+Alt+T"), [this] { trackMask(s_->primaryClip()); });
    act(clip, tr("Stabilize"), {}, [this] { stabilize(s_->primaryClip()); });
    act(clip, tr("Auto Reframe"), {}, [this] { reframe(s_->primaryClip()); });
    act(clip, tr("Speed Ramp..."), {}, [this] {
        QDialog d(this);
        d.setWindowTitle(tr("Speed Ramp"));
        auto* form = new QFormLayout(&d);
        auto* note = new QLabel(tr("Slow motion centred on the playhead, easing in and out of it."), &d);
        note->setWordWrap(true);
        form->addRow(note);
        auto* slow = new QDoubleSpinBox(&d);
        slow->setRange(10, 90);
        slow->setSuffix(" %");
        slow->setDecimals(0);
        auto* len = new QDoubleSpinBox(&d);
        len->setRange(0.1, 10);
        len->setSingleStep(0.1);
        len->setSuffix(tr(" s of footage"));
        auto* ramp = new QDoubleSpinBox(&d);
        ramp->setRange(0.05, 3);
        ramp->setSingleStep(0.05);
        ramp->setSuffix(" s");
        QSettings st;
        slow->setValue(st.value("ramp/slow", 25).toDouble());
        len->setValue(st.value("ramp/length", 1.0).toDouble());
        ramp->setValue(st.value("ramp/ramp", 0.4).toDouble());
        form->addRow(tr("Slow speed"), slow);
        form->addRow(tr("Slow part"), len);
        form->addRow(tr("Ramp in / out"), ramp);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        form->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        st.setValue("ramp/slow", slow->value());
        st.setValue("ramp/length", len->value());
        st.setValue("ramp/ramp", ramp->value());
        s_->speedRampAtPlayhead(slow->value() / 100.0, secondsToTicks(len->value()), secondsToTicks(ramp->value()));
    });
    act(clip, tr("Enhance Voice"), {}, [this] { s_->enhanceVoice(); });
    act(clip, tr("Synchronize (by Audio)"), {}, [this] { s_->synchronizeSelection(); });
    act(clip, tr("Cut at Scene Changes"), {}, [this] { s_->detectScenes(true); });
    act(clip, tr("Remove Silence..."), {}, [this] {
        const Id src = s_->silenceSourceClip();
        if (!src) {
            statusBar()->showMessage(tr("Select an audio clip (or a clip with linked audio) first."), 5000);
            return;
        }
        QDialog d(this);
        d.setWindowTitle(tr("Remove Silence"));
        auto* form = new QFormLayout(&d);
        auto* thr = new QDoubleSpinBox(&d);
        thr->setRange(-80, -10);
        thr->setSuffix(" dB");
        thr->setDecimals(0);
        auto* minLen = new QDoubleSpinBox(&d);
        minLen->setRange(0.2, 10);
        minLen->setSingleStep(0.1);
        minLen->setSuffix(" s");
        auto* pad = new QDoubleSpinBox(&d);
        pad->setRange(0, 1);
        pad->setSingleStep(0.05);
        pad->setSuffix(" s");
        QSettings st;
        thr->setValue(st.value("silence/threshold", -42).toDouble());
        minLen->setValue(st.value("silence/minLength", 0.7).toDouble());
        pad->setValue(st.value("silence/pad", 0.15).toDouble());
        form->addRow(tr("Quieter than"), thr);
        form->addRow(tr("For at least"), minLen);
        form->addRow(tr("Keep around speech"), pad);
        auto* preview = new QLabel(&d);
        preview->setWordWrap(true);
        form->addRow(preview);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Remove"));
        form->addRow(bb);
        auto update = [=] {
            QString why;
            auto spans = s_->findSilences(src, thr->value(), secondsToTicks(minLen->value()), secondsToTicks(pad->value()), why);
            Tick total = 0;
            for (const auto& r : spans) total += r.b - r.a;
            preview->setText(!why.isEmpty() ? why
                                            : tr("%n pause(s), %1 s will be cut from every track (one undo step).", "", int(spans.size()))
                                                  .arg(ticksToSeconds(total), 0, 'f', 1));
            bb->button(QDialogButtonBox::Ok)->setEnabled(!spans.empty());
        };
        for (auto* sb : {thr, minLen, pad}) connect(sb, &QDoubleSpinBox::valueChanged, &d, update);
        update();
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        st.setValue("silence/threshold", thr->value());
        st.setValue("silence/minLength", minLen->value());
        st.setValue("silence/pad", pad->value());
        s_->removeSilence(src, thr->value(), secondsToTicks(minLen->value()), secondsToTicks(pad->value()));
    });
    act(clip, tr("Auto Color"), {}, [this] { s_->autoColorSelection(); });
    QMenu* pip = clip->addMenu(tr("Picture in Picture"));
    const QString corners[] = {tr("Top Left"), tr("Top Right"), tr("Bottom Left"), tr("Bottom Right")};
    for (int i = 0; i < 4; ++i) pip->addAction(corners[i], this, [this, i] { s_->pictureInPicture(i); });
    pip->addSeparator();
    pip->addAction(tr("Full Frame"), this, [this] { s_->pictureInPicture(4); });
    QMenu* anim = clip->addMenu(tr("Animate"));
    for (int i = 0; i < Session::animationNames().size(); ++i)
        anim->addAction(Session::animationNames()[i], this, [this, i] { s_->animateSelection(i); });
    act(clip, tr("Rename..."), {}, [this] {
        Id id = s_->primaryClip();
        const Clip* c = id ? s_->seq().clip(id) : nullptr;
        if (!c) return;
        bool ok = false;
        QString n = QInputDialog::getText(this, tr("Rename Clip"), tr("Name:"), QLineEdit::Normal, toQ(c->name), &ok);
        if (ok && !n.isEmpty()) s_->renameClip(id, n);
    });
    QMenu* audioMenu = clip->addMenu(tr("Audio"));
    act(audioMenu, tr("Audio Gain..."), QKeySequence("G"), [this] { emit s_->gainDialogRequested(); });
    act(audioMenu, tr("Normalize to -14 LUFS"), {}, [this] { s_->normalizeSelectedAudio(-14.0); });
    act(audioMenu, tr("Fit Music to Video End"), {}, [this] { s_->fitMusicToEnd(); });
    act(clip, tr("Match Frame"), QKeySequence("F"), [this] { s_->matchFrame(); });
    clip->addSeparator();
    act(clip, tr("Add Frame Hold"), {}, [this] { s_->addFrameHold(false); });
    act(clip, tr("Insert Frame Hold Segment"), {}, [this] { s_->addFrameHold(true); });
    clip->addSeparator();
    act(clip, tr("Scale to Frame Size (fit)"), {}, [this] { s_->setClipFit(FitMode::Fit); });
    act(clip, tr("Scale to Fill"), {}, [this] { s_->setClipFit(FitMode::Fill); });
    act(clip, tr("Native Size"), {}, [this] { s_->setClipFit(FitMode::None); });
    clip->addSeparator();
    act(clip, tr("Insert"), QKeySequence(","), [this] { s_->placeFromSource(true); }, "insert");
    act(clip, tr("Overwrite"), QKeySequence("."), [this] { s_->placeFromSource(false); }, "overwrite");
    act(clip, tr("Nudge Left"), QKeySequence("Alt+Left"), [this] { s_->nudge(-1); });
    act(clip, tr("Nudge Right"), QKeySequence("Alt+Right"), [this] { s_->nudge(1); });

    QMenu* seq = menuBar()->addMenu(tr("&Sequence"));
    act(seq, tr("Add Edit"), QKeySequence("Ctrl+K"), [this] { s_->addEditAtPlayhead(false); });
    act(seq, tr("Add Edit to All Tracks"), QKeySequence("Ctrl+Shift+K"), [this] { s_->addEditAtPlayhead(true); });
    act(seq, tr("Add Edits at Markers"), {}, [this] { s_->addEditsAtMarkers(); });
    act(seq, tr("Apply Video Transition"), QKeySequence("Ctrl+D"), [this] { s_->applyDefaultTransition(false); });
    act(seq, tr("Apply Audio Transition"), QKeySequence("Ctrl+Shift+D"), [this] { s_->applyDefaultTransition(true); });
    act(seq, tr("Lift"), QKeySequence(";"), [this] { s_->liftExtract(false); });
    act(seq, tr("Extract"), QKeySequence("'"), [this] { s_->liftExtract(true); });
    act(seq, tr("Ripple Trim Previous Edit to Playhead"), QKeySequence("Q"), [this] { s_->rippleTrimToPlayhead(true); });
    act(seq, tr("Ripple Trim Next Edit to Playhead"), QKeySequence("W"), [this] { s_->rippleTrimToPlayhead(false); });
    act(seq, tr("Close All Gaps"), {}, [this] { s_->closeAllGaps(); });
    QMenu* replay = seq->addMenu(tr("Insert Instant Replay"));
    for (int sec : {3, 5, 10})
        replay->addAction(tr("Last %1 s at 50 %").arg(sec), this, [this, sec] { s_->insertInstantReplay(sec, 0.5); });
    replay->addAction(tr("Last 5 s at 25 %"), this, [this] { s_->insertInstantReplay(5, 0.25); });
    seq->addSeparator();
    recordAct_ = act(seq, tr("Record Voice-over"), QKeySequence("Ctrl+Shift+R"), [this] {
        if (s_->recordingVoiceover()) {
            s_->stopVoiceover(true);
            return;
        }
        QString err;
        if (!s_->startVoiceover(err)) {
            recordAct_->setChecked(false);
            QMessageBox::warning(this, tr("Record Voice-over"), err);
        }
    }, "record");
    recordAct_->setCheckable(true);
    recordAct_->setToolTip(tr("Record the microphone while the timeline plays from the playhead (Space stops)"));
    seq->addSeparator();
    QAction* snap = act(seq, tr("Snap in Timeline"), QKeySequence("S"), [this] { snapAct_->toggle(); });
    (void)snap;
    QAction* scrubAct = act(seq, tr("Audio Scrubbing"), {}, [this] {});
    scrubAct->setCheckable(true);
    scrubAct->setChecked(s_->audioScrubbing);
    connect(scrubAct, &QAction::toggled, this, [this](bool on) {
        s_->audioScrubbing = on;
        QSettings().setValue("audio/scrub", on);
    });
    act(seq, tr("Zoom In"), QKeySequence("="), [this] { timeline_->zoomIn(); });
    act(seq, tr("Zoom Out"), QKeySequence("-"), [this] { timeline_->zoomOut(); });
    act(seq, tr("Zoom to Sequence"), QKeySequence("\\"), [this] { timeline_->zoomToFit(); });
    seq->addSeparator();
    act(seq, tr("Go to Previous Edit Point"), QKeySequence(Qt::Key_Up), [this] { s_->gotoEdit(-1); });
    act(seq, tr("Go to Next Edit Point"), QKeySequence(Qt::Key_Down), [this] { s_->gotoEdit(1); });
    seq->addSeparator();
    act(seq, tr("Add Video Track"), {}, [this] {
        s_->edit(tr("Add Track"), [](Project& p) { edit::addTrack(p.sequence, TrackKind::Video); });
    });
    act(seq, tr("Add Audio Track"), {}, [this] {
        s_->edit(tr("Add Track"), [](Project& p) { edit::addTrack(p.sequence, TrackKind::Audio); });
    });

    seq->addSeparator();
    act(seq, tr("Create Vertical Version (9:16)"), {}, [this] { s_->createVerticalVersion(); });

    QMenu* markers = menuBar()->addMenu(tr("&Markers"));
    act(markers, tr("Mark In"), QKeySequence("I"), [this] { activeMonitor()->markIn(); }, "markin");
    act(markers, tr("Mark Out"), QKeySequence("O"), [this] { activeMonitor()->markOut(); }, "markout");
    act(markers, tr("Go to In"), QKeySequence("Shift+I"), [this] { activeMonitor()->goIn(); });
    act(markers, tr("Go to Out"), QKeySequence("Shift+O"), [this] { activeMonitor()->goOut(); });
    act(markers, tr("Mark Clip"), QKeySequence("X"), [this] { s_->markClipAtPlayhead(); });
    act(markers, tr("Select Clip at Playhead"), QKeySequence("D"), [this] { s_->selectClipAtPlayhead(); });
    act(markers, tr("Clear In and Out"), QKeySequence("Ctrl+Shift+X"), [this] {
        if (sourceActive_) {
            s_->sourceIn = s_->sourceOut = kInvalidTick;
            emit s_->changed();
        } else
            s_->edit(tr("Clear In/Out"), [](Project& p) { p.sequence.inPoint = p.sequence.outPoint = kInvalidTick; });
    });
    markers->addSeparator();
    act(markers, tr("Add Marker"), QKeySequence("M"), [this] { s_->addMarkerAtPlayhead(); }, "marker");
    act(markers, tr("Go to Next Marker"), QKeySequence("Shift+M"), [this] {
        for (const auto& m : s_->seq().markers)
            if (m.t > s_->playhead()) {
                s_->setPlayhead(m.t);
                return;
            }
    });
    act(markers, tr("Add Markers at Beats"), {}, [this] { s_->addBeatMarkers(); });
    act(markers, tr("Add Markers at Highlights"), {}, [this] { s_->addHighlightMarkers(); });
    act(markers, tr("Add Markers at Scene Changes"), {}, [this] { s_->detectScenes(false); });
    act(markers, tr("Copy Markers as YouTube Chapters"), {}, [this] { markers_->copyChapters(); });
    act(markers, tr("Go to Previous Marker"), QKeySequence("Ctrl+Shift+M"), [this] {
        const auto& ms = s_->seq().markers;
        for (auto it = ms.rbegin(); it != ms.rend(); ++it)
            if (it->t < s_->playhead()) {
                s_->setPlayhead(it->t);
                return;
            }
    });

    QMenu* gfx = menuBar()->addMenu(tr("&Graphics"));
    // graphics go on the lowest free video track above everything at the playhead
    auto addGraphic = [this](const QString& label, const std::function<Clip(Id track, Tick t, Tick dur)>& make) {
        const Tick t = s_->playhead();
        Id id = 0;
        s_->edit(label, [&](Project& p) {
            Sequence& sq = p.sequence;
            const Tick dur = kDefaultStillDuration;
            Id target = 0;
            auto vts = sq.tracksOf(TrackKind::Video);
            for (auto it = vts.rbegin(); it != vts.rend(); ++it) {
                bool free = true;
                for (const auto* c : sq.clipsOnTrack((*it)->id))
                    if (c->start < t + dur && c->end() > t) free = false;
                if (free && !(*it)->locked) target = (*it)->id;
                else if (target) break;
            }
            if (!target) target = edit::addTrack(sq, TrackKind::Video);
            id = edit::overwriteClips(sq, {make(target, t, dur)}).front();
        });
        s_->setSelection({id});
        docks_["fxcontrols"]->raise();
    };
    act(gfx, tr("New Text Layer"), QKeySequence("Ctrl+T"), [addGraphic] {
        addGraphic(tr("New Title"), [](Id tr, Tick t, Tick d) { return makeTextClip(tr, t, d, "Title"); });
    }, "text");
    act(gfx, tr("New Color Matte..."), {}, [this, addGraphic] {
        const QColor col = QColorDialog::getColor(QColor(0x1e, 0x1e, 0x1e), this, tr("Color Matte"));
        if (!col.isValid()) return;
        const uint32_t argb = col.rgba();
        addGraphic(tr("New Color Matte"), [argb](Id tr, Tick t, Tick d) { return makeMatteClip(tr, t, d, argb); });
    });
    QMenu* templates = gfx->addMenu(tr("Templates"));
    for (int i = 0; i < Session::titleTemplateNames().size(); ++i)
        templates->addAction(Session::titleTemplateNames()[i], this, [this, i] {
            s_->addTitleTemplate(i);
            docks_["fxcontrols"]->raise();
        });
    act(gfx, tr("New Adjustment Layer"), {}, [addGraphic] {
        addGraphic(tr("New Adjustment Layer"), [](Id tr, Tick t, Tick d) { return makeAdjustmentClip(tr, t, d); });
    });
    act(gfx, tr("New Blur Region"), {}, [addGraphic] {
        // an adjustment layer blurring a rectangle: hide names, chat, notifications
        addGraphic(tr("New Blur Region"), [](Id tr, Tick t, Tick d) {
            Clip c = makeAdjustmentClip(tr, t, d);
            c.name = "Blur Region";
            Effect blur = makeEffect(fx::Blur);
            blur.params["radius"].value = 40;
            Effect mask = makeEffect(fx::Mask);
            mask.params["shape"].value = 1;
            mask.params["w"].value = 30;
            mask.params["h"].value = 16;
            mask.params["feather"].value = 6;
            c.effects = {blur, mask};
            return c;
        });
    });
    act(gfx, tr("New Mosaic Region"), {}, [addGraphic] {
        addGraphic(tr("New Mosaic Region"), [](Id tr, Tick t, Tick d) {
            Clip c = makeAdjustmentClip(tr, t, d);
            c.name = "Mosaic Region";
            Effect mosaic = makeEffect(fx::Mosaic);
            mosaic.params["size"].value = 32;
            Effect mask = makeEffect(fx::Mask);
            mask.params["shape"].value = 1;
            mask.params["w"].value = 30;
            mask.params["h"].value = 16;
            mask.params["feather"].value = 0;
            c.effects = {mosaic, mask};
            return c;
        });
    });
    gfx->addSeparator();
    act(gfx, tr("Add Caption at Playhead"), QKeySequence("Ctrl+Alt+C"), [this] {
        s_->addCaptionAtPlayhead();
        docks_["captions"]->raise();
    }, "caption");
    act(gfx, tr("Import Captions (SRT)..."), {}, [this] { captions_->importSrt(); });
    act(gfx, tr("Export Captions (SRT)..."), {}, [this] { captions_->exportSrt(); });
    act(gfx, tr("Caption Style..."), {}, [this] { captions_->styleDialog(); });

    QMenu* win = menuBar()->addMenu(tr("&Window"));
    for (const char* n : {"source", "program", "timeline", "project", "effects", "fxcontrols", "mixer", "scopes", "captions", "markers", "history", "queue", "tools"})
        win->addAction(docks_[n]->toggleViewAction());
    win->addSeparator();
    act(win, tr("Reset Workspace"), {}, [this] { resetWorkspace(); });
    act(win, tr("Performance HUD"), QKeySequence(Qt::Key_F3), [this] {
        bool on = !program_->hud();
        program_->setHud(on);
        source_->setHud(on);
    });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    act(help, tr("User Guide"), QKeySequence(Qt::Key_F1), [this] {
        // the README, rendered, with a find box
        static QPointer<QDialog> guide;
        if (guide) {
            guide->raise();
            guide->activateWindow();
            return;
        }
        auto* d = new QDialog(this);
        guide = d;
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(tr("Cutline User Guide"));
        d->resize(860, 900);
        auto* v = new QVBoxLayout(d);
        auto* find = new QLineEdit;
        find->setPlaceholderText(tr("Find in the guide (Enter for the next match)"));
        find->setClearButtonEnabled(true);
        auto* text = new QTextBrowser;
        text->setObjectName("guideText");
        text->setOpenExternalLinks(true);
        QFile f(":/guide/README.md");
        if (f.open(QIODevice::ReadOnly)) text->setMarkdown(QString::fromUtf8(f.readAll()));
        text->document()->setDefaultStyleSheet("code { background: #2a2a2a; }");
        v->addWidget(find);
        v->addWidget(text, 1);
        connect(find, &QLineEdit::returnPressed, d, [text, find] {
            if (!text->find(find->text())) {  // wrap around
                text->moveCursor(QTextCursor::Start);
                text->find(find->text());
            }
        });
        d->show();
    });
    act(help, tr("Keyboard Shortcuts"), {}, [this] {
        ShortcutsDialog d(findChildren<QAction*>(), this);
        if (d.exec() == QDialog::Accepted) d.apply();
    });
    act(help, tr("About Cutline"), {}, [this] {
        QMessageBox::about(this, tr("About Cutline"),
                           tr("<h3>Cutline %3</h3><p>A fast, offline, GPU-accelerated video editor for Windows.</p>"
                              "<p>Free software under the GNU General Public License v3 or later.<br>"
                              "Source code: <a href='https://github.com/CainanNevils/cutline'>github.com/CainanNevils/cutline</a></p>"
                              "<p>Qt %1 (LGPL v3), FFmpeg %2 (GPL v3 build, includes x264). "
                              "See THIRD_PARTY_NOTICES.txt next to Cutline.exe.</p>")
                               .arg(qVersion())
                               .arg("8.1")
                               .arg(CUTLINE_VERSION));
    });

    // transport (no menu entries; window-wide shortcuts)
    auto key = [this](const QKeySequence& k, std::function<void()> fn, const QString& name) {
        auto* a = new QAction(name, this);
        QString id = name;
        a->setObjectName("cmd.play." + id.remove(QRegularExpression("[^A-Za-z0-9]+")));
        a->setProperty("defaultShortcut", k);
        a->setShortcut(k);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, std::move(fn));
        addAction(a);
    };
    key(QKeySequence(Qt::Key_Space), [this] { s_->togglePlay(activeViewer()); }, tr("Play/Stop"));
    key(QKeySequence("L"), [this] { s_->shuttle(activeViewer(), 1); }, tr("Shuttle Forward"));
    key(QKeySequence("J"), [this] { s_->shuttle(activeViewer(), -1); }, tr("Shuttle Reverse"));
    key(QKeySequence("K"), [this] { s_->shuttle(activeViewer(), 0); }, tr("Shuttle Stop"));
    key(QKeySequence(Qt::Key_Left), [this] { activeMonitor()->step(-1); }, tr("Step Back"));
    key(QKeySequence(Qt::Key_Right), [this] { activeMonitor()->step(1); }, tr("Step Forward"));
    key(QKeySequence("Shift+Left"), [this] { activeMonitor()->step(-5); }, tr("Step Back 5 Frames"));
    key(QKeySequence("Shift+Right"), [this] { activeMonitor()->step(5); }, tr("Step Forward 5 Frames"));
    key(QKeySequence(Qt::Key_Home), [this] {
        if (sourceActive_) s_->setSourcePosition(0);
        else s_->setPlayhead(0);
    }, tr("Go to Start"));
    key(QKeySequence(Qt::Key_End), [this] {
        if (sourceActive_) s_->setSourcePosition(s_->sourceDuration());
        else s_->setPlayhead(std::max<Tick>(0, s_->seq().duration() - s_->seq().frameDur()));
    }, tr("Go to End"));
    applyShortcutOverrides(findChildren<QAction*>());
}

void MainWindow::updateTitle() {
    setWindowTitle(QString("%1%2 - Cutline").arg(s_->displayName()).arg(s_->dirty() ? "*" : ""));
}

bool MainWindow::save(bool saveAs) {
    QString path = s_->projectPath();
    if (saveAs || path.isEmpty()) {
        QSettings st;
        path = QFileDialog::getSaveFileName(this, tr("Save Project"),
                                            st.value("project/dir").toString() + "/" + s_->displayName() + ".cutproj",
                                            tr("Cutline Project (*.cutproj)"));
        if (path.isEmpty()) return false;
        st.setValue("project/dir", QFileInfo(path).absolutePath());
    }
    QString err;
    if (!s_->saveProject(path, err)) {
        QMessageBox::critical(this, tr("Save"), err);
        return false;
    }
    return true;
}

bool MainWindow::maybeSave() {
    if (!s_->dirty()) return true;
    auto r = QMessageBox::question(this, tr("Cutline"), tr("Save changes to %1?").arg(s_->displayName()),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save(false);
    return true;
}

void MainWindow::newProject() {
    if (!maybeSave()) return;
    SequenceSettingsDialog d(s_->seq().settings, this);
    d.setWindowTitle(tr("New Project - Sequence Settings"));
    if (d.exec() != QDialog::Accepted) return;
    s_->newProject(d.settings());
}

void MainWindow::openProjectPath(const QString& path) {
    if (!maybeSave()) return;
    QString err;
    if (!s_->openProject(path, err)) QMessageBox::critical(this, tr("Open Project"), err);
    timeline_->zoomToFit();
}

void MainWindow::newSequenceDialog() {
    SequenceSettingsDialog d(s_->seq().settings, this);
    d.setWindowTitle(tr("New Sequence"));
    if (d.exec() != QDialog::Accepted) return;
    s_->newSequence(d.settings());
    timeline_->zoomToFit();
}

void MainWindow::renameSequenceDialog(int tab) {
    const QString cur = s_->sequenceNames().value(tab);
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Rename Sequence"), tr("Name:"), QLineEdit::Normal, cur, &ok);
    if (ok && !n.trimmed().isEmpty() && n != cur) s_->renameSequence(tab, n);
}

void MainWindow::openProject() {
    if (!maybeSave()) return;
    QSettings st;
    QString f = QFileDialog::getOpenFileName(this, tr("Open Project"), st.value("project/dir").toString(),
                                             tr("Cutline Project (*.cutproj)"));
    if (f.isEmpty()) return;
    st.setValue("project/dir", QFileInfo(f).absolutePath());
    QString err;
    if (!s_->openProject(f, err)) QMessageBox::critical(this, tr("Open Project"), err);
    timeline_->zoomToFit();
}

void MainWindow::startup() {
    QString rec = s_->recoveryCandidate();
    if (!rec.isEmpty() &&
        QMessageBox::question(this, tr("Recover"),
                              tr("Cutline did not shut down properly last time.\nRecover the most recent autosave (%1)?")
                                  .arg(QFileInfo(rec).lastModified().toString("yyyy-MM-dd hh:mm"))) == QMessageBox::Yes) {
        QString err;
        if (!s_->recover(rec, err)) QMessageBox::warning(this, tr("Recover"), err);
        else statusBar()->showMessage(tr("Recovered from autosave. Use File > Save As to keep it."), 10000);
    }
    timeline_->zoomToFit();
}

void MainWindow::trackMask(uint64_t clip) {
    if (!s_->canTrackMask(clip)) {
        s_->trackMaskForward(clip);  // (explains what to select)
        return;
    }
    QProgressDialog pd(tr("Tracking the region..."), tr("Stop"), 0, 1000, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(300);
    s_->trackMaskForward(clip, [&pd](double f) {
        pd.setValue(int(f * 1000));
        QApplication::processEvents();
        return !pd.wasCanceled();
    });
}

void MainWindow::stabilize(uint64_t clip) {
    QProgressDialog pd(tr("Analysing the shake..."), tr("Cancel"), 0, 1000, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(300);
    s_->stabilizeClip(clip, 0.6, [&pd](double f) {
        pd.setValue(int(f * 1000));
        QApplication::processEvents();
        return !pd.wasCanceled();
    });
}

void MainWindow::reframe(uint64_t clip) {
    QProgressDialog pd(tr("Finding the action..."), tr("Cancel"), 0, 1000, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(300);
    s_->autoReframe(clip, [&pd](double f) {
        pd.setValue(int(f * 1000));
        QApplication::processEvents();
        return !pd.wasCanceled();
    });
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (s_->exportQueue()->busy() &&
        QMessageBox::question(this, tr("Quit"), tr("Exports are still running or queued. Quit and cancel them?")) != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings st;
    st.setValue("window/geometry", saveGeometry());
    st.setValue("window/state", saveState());
    st.setValue("window/layoutVersion", 3);
    s_->stop();
    e->accept();
}

} // namespace cut::ui
