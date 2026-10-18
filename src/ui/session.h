#pragma once
// Application controller: owns the document (model + undo), the playback engine, background
// jobs and caches, selection and playhead. Panels talk to the model only through this object;
// every change emits `changed()` and republishes an immutable snapshot to the engine.
#include "core/undo.h"
#include "engine/job_pool.h"
#include "engine/media_cache.h"
#include "engine/playback.h"

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace cut {
class AudioDeviceWatcher;
class AudioRecorder;
}

namespace cut::ui {
class ExportQueue;
}

namespace cut::ui {

struct ProxyState {
    enum Status { None, Queued, Building, Ready, Failed } status = None;
    double progress = 0;
    std::string path;
    MediaInfo info;
    std::string error;
    JobTokenPtr job;
};

class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;
    bool startEngine(QString& error);
    void shutdown();

    Document& doc() { return doc_; }
    const Project& project() const { return doc_.project; }
    const Sequence& seq() const { return doc_.project.sequence; }
    PlaybackEngine* engine() { return engine_.get(); }
    JobPool& jobs() { return *jobs_; }
    MediaCache& cache() { return cache_; }
    int programViewer() const { return programViewer_; }
    int sourceViewer() const { return sourceViewer_; }

    // --- project files ---------------------------------------------------------------------
    void newProject(const SequenceSettings& s);
    bool openProject(const QString& path, QString& error);
    bool saveProject(const QString& path, QString& error);
    QString projectPath() const { return path_; }
    QString displayName() const;
    bool dirty() const { return doc_.dirty(); }
    // Crash recovery: newest autosave if the previous session did not exit cleanly.
    QString recoveryCandidate() const { return recovery_; }
    bool recover(const QString& autosavePath, QString& error);

    // --- assets ------------------------------------------------------------------------------
    // done receives the ids of the assets that were added (on the UI thread).
    void importFiles(const QStringList& files, Id bin = 0, std::function<void(const std::vector<Id>&)> done = {});
    // Import files dropped on the timeline and place them at `at` once probed.
    void importAndPlace(const QStringList& files, Tick at, int vTrack, int aTrack, bool insert);
    QStringList recentProjects() const;
    // Collect Files: copies a project (pass a copy) + its media into destDir (Media\ subfolder); runs
    // on a worker thread; prog reports bytes and can cancel.
    struct CollectProgress {
        std::atomic<uint64_t> done{0}, total{0};
        std::atomic<bool> cancel{false};
        QString projectPath;
    };
    std::vector<Id> usedAssets() const;  // media used by any sequence
    static bool collectFiles(Project p, const QString& projectName, const QString& destDir, bool onlyUsed,
                             CollectProgress& prog, QString& error);
    Id addBin(const QString& name, Id parent = 0);
    void removeAssets(const std::vector<Id>& ids);
    std::vector<Id> unusedAssets() const;  // media no sequence uses (the source monitor's clip stays)
    bool relinkAsset(Id id, const QString& newPath, QString& error);
    int pendingImports() const { return pendingImports_; }

    // --- editing -------------------------------------------------------------------------------
    // Runs `fn` as one undoable step. Returns true if something changed.
    bool edit(const QString& label, const std::function<void(Project&)>& fn);
    // Interactive drags: begin -> update (fn applied to the state at begin) -> end.
    void beginDrag(const QString& label);
    void updateDrag(const std::function<void(Project&)>& fn);
    void endDrag(bool commit);
    bool dragging() const { return dragging_; }
    void undo();
    void redo();

    // --- selection / playhead ---------------------------------------------------------------
    const std::vector<Id>& selection() const { return selection_; }
    void setSelection(std::vector<Id> ids, bool addLinked = true);
    bool isSelected(Id clip) const;
    Id selectedTransition() const { return selTransition_; }
    void setSelectedTransition(Id t);
    Id primaryClip() const;  // the clip Effect Controls shows
    Tick playhead() const { return playhead_; }
    void setPlayhead(Tick t);
    // User scrubbing (ruler drag, scrub bar, frame steps): moves and plays a short audio snippet.
    void scrubPlayhead(Tick t);
    void scrubSource(Tick t);
    bool audioScrubbing = true;
    Tick snapToFrame(Tick t) const;
    enum class Tool { Select, Razor, Ripple, Text, Hand, Zoom, RateStretch };
    Tool tool() const { return tool_; }
    void setTool(Tool t) {
        tool_ = t;
        emit toolChanged();
    }
    bool linkedSelection = true;
    bool snapping = true;
    int targetVideo = 0, targetAudio = 0;

    // --- editing commands (session_commands.cpp) -------------------------------------------------
    std::vector<Id> selectedCaptions;
    Id gapTrack = 0;          // selected gap (Delete ripple-deletes it)
    Tick gapTime = kInvalidTick;
    void deleteSelection(bool ripple);
    void copySelection();
    void cutSelection();
    void paste(bool insert);
    void addEditAtPlayhead(bool allTracks);
    void toggleLink();
    void toggleEnable();
    void addFrameHold(bool segment);
    void addMarkerAtPlayhead();
    void gotoEdit(int dir);
    void applyDefaultTransition(bool audio, int type = -1);
    void selectAll();
    void addCaptionAtPlayhead();
    void nudge(int frames);
    void setClipFit(FitMode m);
    void liftExtract(bool extract);  // sequence In-Out on all unlocked tracks
    void rippleTrimToPlayhead(bool previous);  // Q / W
    void closeAllGaps();
    // Sequences, in tab order (the open one is Project::sequence)
    QStringList sequenceNames() const;
    std::vector<Id> sequenceUids() const;  // same order
    int activeSequence() const { return project().activeSequence; }
    void switchSequence(int tab);
    void newSequence(const SequenceSettings& s, const QString& name = {});
    void duplicateSequence();
    void renameSequence(int tab, const QString& name);
    bool deleteSequence(int tab);
    void nestSelection();               // selected clips -> new sequence + nested clip
    void createVerticalVersion();       // 1080x1920 sequence showing this one (Fill), for Shorts/TikTok
    // a clip wider than its (taller) sequence: slide it so the most motion stays in frame
    int autoReframe(Id clip, const std::function<bool(double)>& progress = {});
    void animateSelection(int preset);  // edit::Animation keyframes on the selected clips
    int autoColorSelection();           // white balance / exposure / contrast from the frame
    // the last `seconds` before the playhead again in slow motion, with a REPLAY title over it
    bool insertInstantReplay(double seconds, double speed = 0.5);
    // the selected music clip lasts until the last video ends: cut + fade out, or looped
    int fitMusicToEnd();
    int pictureInPicture(int corner);
    int replaceWithSourceClip();  // the selected clips show the Source monitor clip (from its In)
    // Blur / Mosaic Region: follow what is under its Shape Mask from the playhead on (Center X/Y
    // keyframes). progress(0..1) returns false to stop. Returns the number of keyframes written.
    int trackMaskForward(Id clip, const std::function<bool(double)>& progress = {});
    bool canTrackMask(Id clip) const;
    // Stabilize (translation): tracks the middle of the clip's picture, smooths the path and
    // counters the shake with Position keys and a small zoom. Returns keys written (0 = failed).
    int stabilizeClip(Id clip, double smoothSeconds = 0.6, const std::function<bool(double)>& progress = {});
    // Graphics > Templates: 0 Lower Third, 1 Subscribe Reminder, 2 Countdown 3-2-1 (at the playhead)
    std::vector<Id> addTitleTemplate(int which);
    static QStringList titleTemplateNames();
    // zoom the clip toward a sequence point from the playhead (Program monitor right-click)
    bool punchIn(Id clip, double x, double y, double factor);   // 0 TL, 1 TR, 2 BL, 3 BR (30 %, rounded, shadow), 4 = full frame
    // Remove Silence: the audio clip to analyse (from the selection), its pauses, and the cut
    Id silenceSourceClip() const;
    std::vector<TickRange> findSilences(Id clipId, double thresholdDb, Tick minLength, Tick pad, QString& why) const;
    int removeSilence(Id clipId, double thresholdDb, Tick minLength, Tick pad);
    int addBeatMarkers();  // markers at the beats of the selected music clip
    int addEditsAtMarkers();  // Ctrl+K at every marker (inside In/Out when set) on unlocked tracks
    int addHighlightMarkers();  // markers at the loudest moments of the selected clip
    int enhanceVoice();    // voice effect chain on the selected mic clips
    int synchronizeSelection();  // line the selected clips up by their audio (earliest one stays)
    // scene changes of the selected video clip (background job): markers, or cuts when cut is set
    void detectScenes(bool cut);
    void applyScenes(const Clip& clip, const std::vector<Tick>& mediaTimes, bool cut);
    static QStringList animationNames();  // in edit::Animation order
    // slow motion centred on the playhead (selected or topmost clip), eased in and out
    bool speedRampAtPlayhead(double slow, Tick slowSource, Tick rampSource);
    void openNested(Id clipId);         // switch to the sequence a nested clip shows
    int tabOfSequence(Id uid) const;    // -1 if gone
    bool canNest(Id uid) const;         // false if it would make a sequence contain itself
    // the sequence as a nested clip (video + linked audio) in the open one
    std::vector<Id> placeSequence(Id uid, Tick at, int vTrack, int aTrack, bool insert);
    // Voice-over: records the input chosen in Preferences (default microphone) while the timeline
    // plays from the playhead; stopping (or playback stopping) places the take on a free audio
    // track, lined up with the picture. CUTLINE_RECORD_LOOPBACK=1 records the output device instead.
    bool startVoiceover(QString& error);
    void stopVoiceover(bool keep = true);
    bool recordingVoiceover() const;
    float voiceoverPeak();          // since the last call
    double voiceoverSeconds() const;
    Id lastVoiceoverClip() const { return lastVoiceover_; }
    const Clip* clipAtPlayhead() const;  // targeted video track first, then top-down
    void markClipAtPlayhead();            // X: sequence In/Out = that clip
    void selectClipAtPlayhead();          // D
    // Effect presets (saved per user). applyEffect: one undo step; returns the clips changed.
    QStringList effectPresets() const;
    bool effectPreset(const QString& name, Effect& out) const;  // "look:<name>" = a built-in look
    static QStringList lookNames();  // built-in Color Correction looks
    void saveEffectPreset(const QString& name, const Effect& e);
    void removeEffectPreset(const QString& name);
    int applyEffect(const std::vector<Id>& ids, const Effect& e, const QString& label);
    // Copied clip's (Ctrl+C) attributes onto the selected clips.
    void pasteAttributes(bool motion, bool opacity, bool effects, bool volume);
    bool hasClipboardClip() const { return !clipboard_.empty(); }
    void matchFrame();               // open the clip under the playhead in Source at that frame
    bool exportFrame(const QString& path, QString& error);  // still of the playhead frame (originals)
    // YouTube thumbnail: the playhead frame at 1280x720 (fills the frame), JPEG under 2 MB
    bool exportThumbnail(const QString& path, QString& error);
    void normalizeSelectedAudio(double targetLufs = -14.0);
    void setSelectedGain(double db, bool relative);
    void renameClip(Id id, const QString& name);
    void setSelectedLabel(uint32_t argb);  // 0 = default colour by kind
    // Emits sequenceMismatch() if this is the first media in the sequence and its size/fps differ.
    void checkSequenceMatch(Id asset, bool sequenceWasEmpty);

    // --- playback ------------------------------------------------------------------------------
    void togglePlay(int viewer);
    void shuttle(int viewer, int dir);  // J/K/L
    void stop();
    bool playing() const;

    // --- source monitor --------------------------------------------------------------------------
    Id sourceAsset() const { return sourceAsset_; }
    void openInSource(Id asset);
    Tick sourceIn = kInvalidTick, sourceOut = kInvalidTick;
    Tick sourcePosition() const { return sourcePos_; }
    void setSourcePosition(Tick t);
    Tick sourceDuration() const;
    Rational sourceFps() const;
    // Insert (',') / overwrite ('.') the source range at the playhead on the target tracks.
    void placeFromSource(bool insert, bool video = true, bool audio = true);

    // --- caches / proxies ----------------------------------------------------------------------------
    ThumbStripPtr thumbs(Id asset) const;
    WaveformPtr waveform(Id asset, int stream) const;
    const ProxyState* proxy(Id asset) const;
    bool proxiesEnabled() const { return proxiesEnabled_; }
    void setProxiesEnabled(bool on);
    void buildProxy(Id asset);
    void setExporting(bool on);
    bool exporting() const { return exporting_; }
    ExportQueue* exportQueue() { return exportQueue_.get(); }

    // --- preferences ------------------------------------------------------------------------------------
    void applyPreferences();
    void autosaveNow() { autosave(); }
    bool restartEngine(QString& error);
    SnapshotPtr snapshot(bool forExport = false) const;

signals:
    void changed();             // model changed (repaint timeline, panels)
    void structureChanged();    // tracks/assets/bins changed (rebuild lists)
    void selectionChanged();
    void playheadChanged(qint64 t);
    void sourceChanged();
    void sourcePositionChanged(qint64 t);
    void mediaUpdated();        // thumbnails / waveforms / proxy progress
    void playStateChanged(bool playing);
    void message(const QString& text, int timeoutMs = 4000);
    void titleChanged();
    void engineError(const QString& text);
    void engineRestarted();
    void toolChanged();
    void speedDialogRequested();
    void projectLoaded();
    void hdrImported(const QString& names);
    void sequenceMismatch(Id asset);
    void recentChanged();
    void gainDialogRequested();
    void scopeFrame(int viewer, const QImage& rgba);
    void effectPresetsChanged();
    void voiceoverChanged(bool recording);

private:
    void publish();
    void publishSource();
    void afterChange(bool structure);
    void loadMediaCaches(const Asset& a);
    void scheduleMediaJobs(const Asset& a);
    void autosave();
    void addRecent(const QString& path);
    void checkRecovery();
    void markOffline();

    Document doc_;
    std::unique_ptr<PlaybackEngine> engine_;
    std::unique_ptr<JobPool> jobs_;
    std::unique_ptr<AudioDeviceWatcher> deviceWatcher_;
    std::map<int, Tick> seqPlayheads_;  // playhead per sequence tab
    QTimer deviceTimer_;
    std::string lastDefaultDevice_;
    bool lastChosenPresent_ = true;
    void audioDevicesChanged();
    MediaCache cache_;
    int programViewer_ = 0, sourceViewer_ = 1;
    QString path_;
    std::vector<Id> selection_;
    Id selTransition_ = 0;
    Tick playhead_ = 0;
    bool dragging_ = false;
    Tool tool_ = Tool::Select;
    std::string dragStructure_;
    Id sourceAsset_ = 0;
    Tick sourcePos_ = 0;
    bool proxiesEnabled_ = false;
    bool exporting_ = false;
    int pendingImports_ = 0;

    std::unique_ptr<ExportQueue> exportQueue_;
    std::unique_ptr<AudioRecorder> recorder_;
    QString voPath_;
    Tick voStart_ = 0;
    int voTrack_ = 0;
    std::vector<int64_t> voOffsets_;  // timeline ticks minus wall clock (as ticks), sampled while recording
    QTimer voTimer_;
    Id lastVoiceover_ = 0;

    mutable std::map<Id, ThumbStripPtr> thumbs_;
    mutable std::map<std::pair<Id, int>, WaveformPtr> waves_;
    std::map<Id, ProxyState> proxies_;
    std::set<std::string> scheduled_;  // cache keys with jobs queued
    std::vector<Clip> clipboard_;
    std::vector<Caption> clipboardCaptions_;
    Tick clipboardOrigin_ = 0;

    QTimer autosaveTimer_;
    QTimer mediaTimer_;  // progressive waveform / proxy progress repaint
    int autosaveIndex_ = 0;
    QString recovery_;
    QString lockPath_;
};

QString toQ(const std::string& s);
std::string toS(const QString& s);

} // namespace cut::ui
