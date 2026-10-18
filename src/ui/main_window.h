#pragma once
#include <QMainWindow>

#include <map>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QTabBar;

namespace cut::ui {

class Session;
class MonitorPanel;
class TimelineWidget;
class ProjectPanel;
class EffectsPanel;
class EffectControls;
class AudioMixerPanel;
class CaptionsPanel;
class ScopesPanel;
class HistoryPanel;
class MarkersPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Session* s);
    void startup();  // recovery prompt / initial project

protected:
    void closeEvent(QCloseEvent*) override;

private:
    void buildDocks();
    void buildMenus();
    void resetWorkspace();
    void updateTitle();
    bool maybeSave();
    bool save(bool saveAs);
    void openProject();
    void openProjectPath(const QString& path);
    void newSequenceDialog();
    void renameSequenceDialog(int tab);
    void newProject();
    MonitorPanel* activeMonitor() const;
    int activeViewer() const;
    QAction* act(QMenu* m, const QString& text, const QKeySequence& key, std::function<void()> fn,
                 const QString& iconName = {});

    Session* s_;
    MonitorPanel* source_;
    MonitorPanel* program_;
    TimelineWidget* timeline_;
    ProjectPanel* project_;
    EffectsPanel* effects_;
    EffectControls* fxControls_;
    AudioMixerPanel* mixer_;
    CaptionsPanel* captions_;
    ScopesPanel* scopes_;
    HistoryPanel* history_;
    MarkersPanel* markers_;
    QTabBar* seqTabs_ = nullptr;
    std::map<QString, QDockWidget*> docks_;
    QByteArray defaultState_;
    QLabel* statusDecode_;
    QLabel* statusJobs_;
    QLabel* statusRec_ = nullptr;
public:
    void trackMask(uint64_t clip);  // Track Mask Forward with a progress dialog (clip Id)
    void stabilize(uint64_t clip);
    void reframe(uint64_t clip);
private:  // voice-over take: time + input level
    QAction* recordAct_ = nullptr;
    QActionGroup* tools_;
    QAction* snapAct_;
    bool sourceActive_ = false;
};

} // namespace cut::ui
