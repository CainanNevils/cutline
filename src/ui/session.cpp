#include "ui/session.h"

#include "audio/audio_output.h"
#include "audio/device_watcher.h"
#include "audio/recorder.h"
#include "core/edits.h"
#include "core/project_io.h"
#include "export/proxy.h"
#include "ui/export_queue.h"
#include "media/probe.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

namespace cut::ui {

QString toQ(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
std::string toS(const QString& s) {
    QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
}

namespace {
QString autosaveDir() {
    QString d = toQ(appDataDir()) + "/autosave";
    QDir().mkpath(d);
    return d;
}
} // namespace

Session::Session(QObject* parent) : QObject(parent) {
    jobs_ = std::make_unique<JobPool>();
    exportQueue_ = std::make_unique<ExportQueue>(this);
    doc_.reset(Project{});
    doc_.project.sequence = makeDefaultSequence();
    applyPreferences();
    audioScrubbing = QSettings().value("audio/scrub", true).toBool();
    jobs_->submit("cache purge", [this](JobToken&) { cache_.purge(); });
    connect(&autosaveTimer_, &QTimer::timeout, this, [this] { autosave(); });
    autosaveTimer_.start(60'000);
    connect(&mediaTimer_, &QTimer::timeout, this, [this] {
        if (jobs_->running() || jobs_->pending()) emit mediaUpdated();
    });
    mediaTimer_.start(250);
    connect(this, &Session::playStateChanged, this, [this](bool p) {  // Space ends a voice-over take
        if (!p && recordingVoiceover() && engine_ && !engine_->isPlaying()) stopVoiceover(true);
    });
    connect(&voTimer_, &QTimer::timeout, this, [this] {  // timeline/wall clock pairs for lining the take up
        int64_t wall = 0;
        Tick t = 0;
        if (engine_ && engine_->clockSample(wall, t)) voOffsets_.push_back(t - (wall * (kTicksPerSecond / 10000) / 100));
    });
    checkRecovery();
    // follow the Windows default output (headphones plugged in, USB interface removed)
    if (!qEnvironmentVariableIsSet("CUTLINE_AUDIO_DEVICE") && !qEnvironmentVariableIsSet("CUTLINE_TEST")) {
        deviceTimer_.setSingleShot(true);
        connect(&deviceTimer_, &QTimer::timeout, this, &Session::audioDevicesChanged);
        deviceWatcher_ = std::make_unique<AudioDeviceWatcher>([this] {
            QMetaObject::invokeMethod(this, [this] { deviceTimer_.start(700); });  // events come in bursts
        });
    }
}

void Session::audioDevicesChanged() {
    if (!engine_) return;
    if (exporting_ || dragging_) {
        deviceTimer_.start(2000);
        return;
    }
    const QString chosen = QSettings().value("audio/device").toString();
    const std::string def = defaultAudioDeviceId();
    bool chosenPresent = false;
    for (const auto& [id, name] : listAudioDevices()) chosenPresent |= toQ(id) == chosen;
    const AudioStatus au = engine_->audioStatus();
    // restart when following the default and it moved, when the chosen device came back or went
    // away, or when audio is not running at all
    const bool restart = !au.ok || (chosen.isEmpty() && def != lastDefaultDevice_) ||
                         (!chosen.isEmpty() && chosenPresent != lastChosenPresent_);
    lastDefaultDevice_ = def;
    lastChosenPresent_ = chosenPresent;
    if (!restart) return;
    stop();
    QString err;
    if (restartEngine(err)) emit message(tr("Audio output: %1").arg(toQ(engine_->audioStatus().device)), 5000);
    else emit engineError(err);
}

Session::~Session() { shutdown(); }

void Session::shutdown() {
    if (recorder_) recorder_->stop();
    if (exportQueue_) exportQueue_->cancelAll();
    if (jobs_) jobs_->cancelAll();
    if (engine_) engine_->shutdown();
    engine_.reset();
    jobs_.reset();
    if (!lockPath_.isEmpty()) QFile::remove(lockPath_);  // clean exit: no recovery prompt next time
    lockPath_.clear();
}

void Session::checkRecovery() {
    if (qEnvironmentVariableIsSet("CUTLINE_TEST")) return;  // tools/tests never touch the app's lock
    lockPath_ = toQ(appDataDir()) + "/session.lock";
    if (QFile::exists(lockPath_)) {
        QFileInfoList l = QDir(autosaveDir()).entryInfoList({"*.cutproj"}, QDir::Files, QDir::Time);
        if (!l.isEmpty()) recovery_ = l.first().absoluteFilePath();
    }
    QFile f(lockPath_);
    if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::number(QCoreApplication::applicationPid()));
}

bool Session::startEngine(QString& error) {
    engine_ = std::make_unique<PlaybackEngine>();
    engine_->setAudioDevice(QSettings().value("audio/device").toString().toStdString());
    lastDefaultDevice_ = defaultAudioDeviceId();
    {
        const QString chosen = QSettings().value("audio/device").toString();
        lastChosenPresent_ = chosen.isEmpty();
        for (const auto& [id, name] : listAudioDevices()) lastChosenPresent_ |= toQ(id) == chosen;
    }
    std::string err;
    if (!engine_->start(err)) {
        error = toQ(err);
        engine_.reset();
        return false;
    }
    engine_->setFrameBudget(size_t(QSettings().value("gpu/budgetMB", 3072).toInt()) << 20);
    programViewer_ = engine_->addViewer();
    sourceViewer_ = engine_->addViewer();
    engine_->onPosition = [this](int viewer, Tick t) {
        QMetaObject::invokeMethod(this, [this, viewer, t] {
            if (viewer == programViewer_) {
                playhead_ = t;
                emit playheadChanged(t);
            } else {
                sourcePos_ = t;
                emit sourcePositionChanged(t);
            }
        });
    };
    engine_->onScope = [this](int viewer, int w, int h, std::vector<uint8_t> px) {
        QImage img(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y) std::memcpy(img.scanLine(y), px.data() + size_t(y) * w * 4, size_t(w) * 4);
        QMetaObject::invokeMethod(this, [this, viewer, img] { emit scopeFrame(viewer, img); });
    };
    engine_->onStopped = [this](int) { QMetaObject::invokeMethod(this, [this] { emit playStateChanged(false); }); };
    engine_->onPlayStateChanged = [this](bool p) {
        jobs_->setPaused(p || exporting_);
        QMetaObject::invokeMethod(this, [this, p] { emit playStateChanged(p); });
    };
    engine_->onError = [this](const std::string& e) {
        QMetaObject::invokeMethod(this, [this, e] { emit engineError(toQ(e)); });
    };
    publish();
    publishSource();
    return true;
}

bool Session::restartEngine(QString& error) {
    stopVoiceover(true);
    if (engine_) engine_->shutdown();
    engine_.reset();
    if (!startEngine(error)) return false;
    emit engineRestarted();
    return true;
}

void Session::applyPreferences() {
    QSettings s;
    cache_.setDir(toS(s.value("cache/dir").toString()));
    cache_.setCapBytes(uint64_t(s.value("cache/capGB", 20).toInt()) << 30);
    if (engine_) engine_->setFrameBudget(size_t(s.value("gpu/budgetMB", 3072).toInt()) << 20);
}

// ---------------------------------------------------------------------------------------------
// Snapshots

SnapshotPtr Session::snapshot(bool forExport) const {
    auto s = std::make_shared<RenderSnapshot>();
    s->seq = doc_.project.sequence;
    s->revision = doc_.revision();
    s->useProxies = proxiesEnabled_ && !forExport;
    for (const auto& a : doc_.project.assets) {
        AssetRef r;
        r.id = a.id;
        r.path = a.path;
        r.info = a.info;
        r.offline = a.offline;
        auto it = proxies_.find(a.id);
        if (!forExport && it != proxies_.end() && it->second.status == ProxyState::Ready) {
            r.proxyPath = it->second.path;
            r.proxyInfo = it->second.info;
        }
        s->assets[a.id] = r;
    }
    addNestedSnapshots(*s, doc_.project);  // sequences shown by nested clips
    return s;
}

void Session::publish() {
    if (engine_) engine_->setContent(programViewer_, snapshot());
}

void Session::publishSource() {
    if (!engine_) return;
    const Asset* a = doc_.project.asset(sourceAsset_);
    auto s = std::make_shared<RenderSnapshot>();
    if (a) {
        SequenceSettings ss;
        if (a->info.hasVideo) {
            ss.width = std::max(2, a->info.video.width);
            ss.height = std::max(2, a->info.video.height);
            ss.fps = a->info.isImage ? Rational{30, 1} : a->info.video.fps.valid() ? a->info.video.fps : Rational{30, 1};
        } else {
            ss.width = 1280;
            ss.height = 720;
            ss.fps = {30, 1};
        }
        s->seq = makeDefaultSequence(ss);
        Tick dur = a->info.isImage ? kDefaultStillDuration : a->info.duration;
        auto clips = edit::clipsForAsset(s->seq, *a, 0, 0, dur, 0, 0);
        edit::overwriteClips(s->seq, clips);
        AssetRef r;
        r.id = a->id;
        r.path = a->path;
        r.info = a->info;
        r.offline = a->offline;
        auto it = proxies_.find(a->id);
        if (it != proxies_.end() && it->second.status == ProxyState::Ready) {
            r.proxyPath = it->second.path;
            r.proxyInfo = it->second.info;
        }
        s->assets[a->id] = r;
        s->useProxies = proxiesEnabled_;
    }
    engine_->setContent(sourceViewer_, s);
}

void Session::afterChange(bool structure) {
    // drop selection entries that no longer exist
    auto& sel = selection_;
    sel.erase(std::remove_if(sel.begin(), sel.end(), [&](Id id) { return !seq().clip(id); }), sel.end());
    if (selTransition_ && !doc_.project.sequence.transition(selTransition_)) selTransition_ = 0;
    publish();
    if (structure) emit structureChanged();
    emit changed();
    emit titleChanged();
}

// ---------------------------------------------------------------------------------------------
// Editing

namespace {
// Changes that require panels to rebuild their lists (tracks, assets, bins), as opposed to
// clip-level edits that only need a repaint.
std::string structureKey(const Project& p) {
    std::string k;
    for (const auto& t : p.sequence.tracks) k += std::to_string(t.id) + t.name + ",";
    k += "|";
    for (const auto& a : p.assets) k += std::to_string(a.id) + a.name + a.path + std::to_string(a.binId) + std::to_string(a.offline) + ",";
    k += "|";
    for (const auto& b : p.bins) k += std::to_string(b.id) + b.name + std::to_string(b.parent) + ",";
    k += "|" + p.sequence.name + std::to_string(p.sequence.uid) + "@" + std::to_string(p.activeSequence);
    for (const auto& s : p.otherSequences) k += "," + s.name + std::to_string(s.uid);  // (Project panel lists them)
    return k;
}
} // namespace

bool Session::edit(const QString& label, const std::function<void(Project&)>& fn) {
    if (dragging_) endDrag(true);
    const std::string before = structureKey(doc_.project);
    bool ch = doc_.edit(toS(label), fn);
    if (ch) afterChange(structureKey(doc_.project) != before);
    return ch;
}
void Session::beginDrag(const QString& label) {
    if (dragging_) endDrag(true);
    dragStructure_ = structureKey(doc_.project);
    doc_.begin(toS(label));
    dragging_ = true;
}

void Session::updateDrag(const std::function<void(Project&)>& fn) {
    if (!dragging_) return;
    doc_.revertToSnapshot();
    fn(doc_.project);
    edit::normalize(doc_.project.sequence);
    doc_.touch();
    publish();
    emit changed();
}

void Session::endDrag(bool commit) {
    if (!dragging_) return;
    dragging_ = false;
    if (commit) doc_.commit();
    else doc_.rollback();
    afterChange(structureKey(doc_.project) != dragStructure_);
}

void Session::undo() {
    if (dragging_) endDrag(false);
    const std::string before = structureKey(doc_.project);
    const QString label = toQ(doc_.undo.undoLabel());
    if (doc_.doUndo()) {
        afterChange(structureKey(doc_.project) != before);
        emit message(tr("Undo: %1").arg(label), 1500);
    }
}

void Session::redo() {
    if (dragging_) endDrag(false);
    const std::string before = structureKey(doc_.project);
    const QString label = toQ(doc_.undo.redoLabel());
    if (doc_.doRedo()) {
        afterChange(structureKey(doc_.project) != before);
        emit message(tr("Redo: %1").arg(label), 1500);
    }
}

// ---------------------------------------------------------------------------------------------
// Selection / playhead / transport

void Session::setSelection(std::vector<Id> ids, bool addLinked) {
    if (addLinked && linkedSelection) {
        std::vector<Id> all;
        for (Id id : ids)
            for (Id l : seq().linkedClips(id))
                if (std::find(all.begin(), all.end(), l) == all.end()) all.push_back(l);
        ids = all;
    }
    // keep clicked clip first (primary)
    if (ids == selection_) return;
    selection_ = std::move(ids);
    if (!selection_.empty()) selTransition_ = 0;
    emit selectionChanged();
    emit changed();
}

bool Session::isSelected(Id clip) const {
    return std::find(selection_.begin(), selection_.end(), clip) != selection_.end();
}

void Session::setSelectedTransition(Id t) {
    selTransition_ = t;
    if (t) selection_.clear();
    emit selectionChanged();
    emit changed();
}

Id Session::primaryClip() const {
    // prefer a visual clip if the selection is a linked A/V pair
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id)) {
            const Track* t = seq().track(c->trackId);
            if (t && t->kind == TrackKind::Video) return id;
        }
    return selection_.empty() ? 0 : selection_.front();
}

Tick Session::snapToFrame(Tick t) const { return cut::snapToFrame(t, seq().settings.fps); }

void Session::setPlayhead(Tick t) {
    t = std::max<Tick>(0, cut::floorToFrame(t, seq().settings.fps));
    if (t == playhead_ && !(engine_ && engine_->isPlaying())) return;
    playhead_ = t;
    if (engine_) engine_->seek(programViewer_, t);
    emit playheadChanged(t);
}

void Session::scrubPlayhead(Tick t) {
    const Tick before = playhead_;
    setPlayhead(t);
    if (audioScrubbing && engine_ && playhead_ != before) engine_->scrubAudio(programViewer_, playhead_);
}

void Session::scrubSource(Tick t) {
    const Tick before = sourcePos_;
    setSourcePosition(t);
    if (audioScrubbing && engine_ && sourcePos_ != before) engine_->scrubAudio(sourceViewer_, sourcePos_);
}

bool Session::playing() const { return engine_ && engine_->isPlaying(); }

void Session::togglePlay(int viewer) {
    if (!engine_) return;
    if (engine_->isPlaying()) {
        engine_->pause();
        return;
    }
    engine_->play(viewer, 1.0);
}

void Session::shuttle(int viewer, int dir) {
    if (!engine_) return;
    if (dir == 0) {
        engine_->pause();
        return;
    }
    double cur = engine_->isPlaying() && engine_->playingViewer() == viewer ? engine_->playSpeed() : 0;
    double sp;
    if (dir > 0) sp = cur >= 1 ? std::min(8.0, cur * 2) : 1.0;
    else sp = cur <= -1 ? std::max(-8.0, cur * 2) : -1.0;
    if (dir < 0) {
        // reverse playback reads proxies (all-intra); build missing ones on demand
        std::set<Id> assets;
        if (viewer == sourceViewer_) assets.insert(sourceAsset_);
        else
            for (const auto& c : seq().clips)
                if (c.kind == ClipKind::Media) assets.insert(c.assetId);
        int building = 0;
        for (Id id : assets) {
            const Asset* a = doc_.project.asset(id);
            if (!a || !a->info.hasVideo || a->info.isImage) continue;
            const ProxyState* ps = proxy(id);
            if (!ps || ps->status != ProxyState::Ready) {
                buildProxy(id);
                ++building;
            }
        }
        if (building) emit message(tr("Generating proxy... reverse playback gets smooth once it is ready."), 4000);
        else if (!proxiesEnabled_) setProxiesEnabled(true);
    }
    engine_->play(viewer, sp);
}

void Session::stop() {
    if (engine_) engine_->pause();
}

// ---------------------------------------------------------------------------------------------
// Source monitor

void Session::openInSource(Id asset) {
    sourceAsset_ = asset;
    sourceIn = sourceOut = kInvalidTick;
    sourcePos_ = 0;
    publishSource();
    if (engine_) engine_->seek(sourceViewer_, 0);
    emit sourceChanged();
    emit sourcePositionChanged(0);
}

void Session::setSourcePosition(Tick t) {
    t = std::clamp<Tick>(cut::floorToFrame(t, sourceFps()), 0, std::max<Tick>(0, sourceDuration() - 1));
    sourcePos_ = t;
    if (engine_) engine_->seek(sourceViewer_, t);
    emit sourcePositionChanged(t);
}

Tick Session::sourceDuration() const {
    const Asset* a = doc_.project.asset(sourceAsset_);
    if (!a) return 0;
    return a->info.isImage ? kDefaultStillDuration : a->info.duration;
}

Rational Session::sourceFps() const {
    const Asset* a = doc_.project.asset(sourceAsset_);
    if (!a || !a->info.hasVideo || a->info.isImage || !a->info.video.fps.valid()) return {30, 1};
    return a->info.video.fps;
}

void Session::placeFromSource(bool insert, bool video, bool audio) {
    const Asset* a = doc_.project.asset(sourceAsset_);
    if (!a) {
        emit message(tr("Open a clip in the Source Monitor first (double-click it in the Project panel)."));
        return;
    }
    Tick in = sourceIn != kInvalidTick ? sourceIn : 0;
    Tick out = sourceOut != kInvalidTick ? sourceOut : sourceDuration();
    if (out <= in) {
        emit message(tr("Source In is after Out."));
        return;
    }
    Asset asset = *a;
    Tick at = playhead_;
    Tick end = at;
    const bool wasEmpty = seq().clips.empty();
    edit(insert ? tr("Insert") : tr("Overwrite"), [&](Project& p) {
        auto clips = edit::clipsForAsset(p.sequence, asset, at, in, out, targetVideo, targetAudio, video, audio);
        for (const auto& c : clips) end = std::max(end, c.end());
        if (insert) edit::insertClips(p.sequence, clips, at);
        else edit::overwriteClips(p.sequence, clips);
    });
    setPlayhead(end);
    checkSequenceMatch(asset.id, wasEmpty);
}

// ---------------------------------------------------------------------------------------------
// Files

QString Session::displayName() const {
    QString n = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).completeBaseName();
    return n;
}

void Session::newProject(const SequenceSettings& s) {
    if (engine_) engine_->pause();
    Project p;
    p.sequence = makeDefaultSequence(s);
    doc_.reset(std::move(p));
    path_.clear();
    selection_.clear();
    selTransition_ = 0;
    playhead_ = 0;
    sourceAsset_ = 0;
    thumbs_.clear();
    waves_.clear();
    proxies_.clear();
    publishSource();
    afterChange(true);
    emit sourceChanged();
    emit playheadChanged(0);
    emit projectLoaded();
}

bool Session::openProject(const QString& path, QString& error) {
    Project p;
    std::string err;
    if (!loadProjectFile(toS(path), p, err)) {
        error = toQ(err);
        return false;
    }
    if (engine_) engine_->pause();
    int offline = resolveMediaPaths(p, toS(path));
    doc_.reset(std::move(p));
    path_ = path;
    selection_.clear();
    selTransition_ = 0;
    playhead_ = 0;
    sourceAsset_ = 0;
    thumbs_.clear();
    waves_.clear();
    proxies_.clear();
    for (const auto& a : doc_.project.assets)
        if (!a.offline) {
            loadMediaCaches(a);
            scheduleMediaJobs(a);
        }
    publishSource();
    afterChange(true);
    emit sourceChanged();
    emit playheadChanged(0);
    emit projectLoaded();
    if (offline > 0) emit message(tr("%1 media file(s) are offline. Use Relink in the Project panel.").arg(offline), 8000);
    addRecent(path);
    return true;
}

bool Session::saveProject(const QString& path, QString& error) {
    std::string err;
    if (!saveProjectFile(doc_.project, toS(path), err)) {
        error = toQ(err);
        return false;
    }
    path_ = path;
    doc_.markSaved();
    addRecent(path);
    emit titleChanged();
    emit message(tr("Saved %1").arg(QFileInfo(path).fileName()), 2500);
    return true;
}

void Session::addRecent(const QString& path) {
    if (qEnvironmentVariableIsSet("CUTLINE_TEST") || path.startsWith(toQ(appDataDir()))) return;  // not autosaves/tests
    QSettings st;
    const QString norm = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    QStringList l = st.value("recent/list").toStringList();
    l.erase(std::remove_if(l.begin(), l.end(), [&](const QString& p) {
                return QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath()).compare(norm, Qt::CaseInsensitive) == 0;
            }),
            l.end());
    l.prepend(norm);
    while (l.size() > 10) l.removeLast();
    st.setValue("recent/list", l);
    emit recentChanged();
}

QStringList Session::recentProjects() const {
    QStringList l;
    for (const QString& p : QSettings().value("recent/list").toStringList()) {
        const QString norm = QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath());
        if (QFileInfo::exists(norm) && !l.contains(norm, Qt::CaseInsensitive)) l << norm;
    }
    return l;
}

bool Session::recover(const QString& autosavePath, QString& error) {
    if (!openProject(autosavePath, error)) return false;
    path_.clear();  // force "Save As" so the autosave file is not overwritten in place
    doc_.touch();
    emit titleChanged();
    return true;
}

void Session::autosave() {
    if (!doc_.dirty() || dragging_ || qEnvironmentVariableIsSet("CUTLINE_TEST")) return;
    QString name = displayName();
    name.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    QString file = autosaveDir() + QString("/%1_%2.cutproj").arg(name).arg(autosaveIndex_++ % 5);
    std::string err;
    std::string projPath = path_.isEmpty() ? toS(file) : toS(path_);
    // relative media paths stay relative to the real project location
    std::string json = serializeProject(doc_.project, projPath);
    if (writeFileAtomic(toS(file), json)) emit message(tr("Autosaved"), 1200);
}

// ---------------------------------------------------------------------------------------------
// Assets, caches, proxies

void Session::importFiles(const QStringList& files, Id bin, std::function<void(const std::vector<Id>&)> done) {
    struct Result {
        int order;
        QString path;
        MediaInfo info;
    };
    auto results = std::make_shared<std::vector<Result>>();
    auto remaining = std::make_shared<int>(int(files.size()));
    pendingImports_ += int(files.size());
    emit message(tr("Importing %1 file(s)...").arg(files.size()), 2000);
    int order = 0;
    for (const QString& f : files) {
        std::string path = toS(QDir::toNativeSeparators(QFileInfo(f).absoluteFilePath()));
        const int idx = order++;
        jobs_->submit(
            "probe",
            [this, path, results, remaining, bin, idx, done](JobToken&) {
                MediaInfo mi = probeMedia(path);
                QMetaObject::invokeMethod(this, [this, path, mi, results, remaining, bin, idx, done] {
                    results->push_back({idx, toQ(path), mi});
                    --pendingImports_;
                    if (--*remaining > 0) return;
                    std::sort(results->begin(), results->end(), [](const Result& a, const Result& b) { return a.order < b.order; });
                    std::vector<Asset> added;
                    QStringList failed;
                    edit(tr("Import"), [&](Project& p) {
                        for (const auto& r : *results) {
                            if (!r.info.valid) {
                                failed << QFileInfo(r.path).fileName() + ": " + toQ(r.info.error);
                                continue;
                            }
                            Asset a;
                            a.id = p.newAssetId();
                            a.binId = bin;
                            a.path = toS(r.path);
                            a.name = toS(QFileInfo(r.path).fileName());
                            a.info = r.info;
                            p.assets.push_back(a);
                            added.push_back(a);
                        }
                    });
                    QStringList hdr;
                    for (const auto& a : added) {
                        loadMediaCaches(a);
                        scheduleMediaJobs(a);
                        if (a.info.hasVideo && a.info.video.transfer != ColorTransfer::SDR) hdr << toQ(a.name);
                    }
                    if (!hdr.isEmpty()) emit hdrImported(hdr.join(", "));
                    if (done) {
                        std::vector<Id> ids;
                        for (const auto& a : added) ids.push_back(a.id);
                        done(ids);
                    }
                    if (!failed.isEmpty()) emit message(tr("Could not import: %1").arg(failed.join("; ")), 10000);
                    else emit message(tr("Imported %1 file(s)").arg(added.size()), 2500);
                    if (sourceAsset_ == 0 && !added.empty()) openInSource(added.front().id);
                });
            },
            JobPool::Lane::General, true);
    }
}

Id Session::addBin(const QString& name, Id parent) {
    Id id = 0;
    edit(tr("New Bin"), [&](Project& p) {
        Bin b;
        b.id = p.newAssetId();
        b.parent = parent;
        b.name = toS(name);
        p.bins.push_back(b);
        id = b.id;
    });
    return id;
}

void Session::removeAssets(const std::vector<Id>& ids) {
    std::set<Id> del(ids.begin(), ids.end());
    edit(tr("Remove from Project"), [&](Project& p) {
        // clips using the media go from every sequence (deleteClips also drops their transitions)
        auto purge = [&](Sequence& s) {
            std::vector<Id> gone;
            for (const auto& c : s.clips)
                if (c.kind == ClipKind::Media && del.count(c.assetId)) gone.push_back(c.id);
            edit::deleteClips(s, gone);
        };
        purge(p.sequence);
        for (auto& o : p.otherSequences) purge(o);
        p.assets.erase(std::remove_if(p.assets.begin(), p.assets.end(), [&](const Asset& a) { return del.count(a.id) > 0; }),
                       p.assets.end());
        // bins: delete selected bins too (their assets go to root)
        for (auto& a : p.assets)
            if (del.count(a.binId)) a.binId = 0;
        p.bins.erase(std::remove_if(p.bins.begin(), p.bins.end(), [&](const Bin& b) { return del.count(b.id) > 0; }),
                     p.bins.end());
    });
    if (del.count(sourceAsset_)) openInSource(0);
}

bool Session::relinkAsset(Id id, const QString& newPath, QString& error) {
    MediaInfo mi = probeMedia(toS(newPath));
    if (!mi.valid) {
        error = toQ(mi.error);
        return false;
    }
    const Asset* a = doc_.project.asset(id);
    if (!a) return false;
    if (a->info.hasVideo && !mi.hasVideo) {
        error = tr("The new file has no video stream.");
        return false;
    }
    edit(tr("Relink Media"), [&](Project& p) {
        Asset* x = p.asset(id);
        x->path = toS(QDir::toNativeSeparators(newPath));
        x->info = mi;
        x->offline = false;
    });
    if (const Asset* x = doc_.project.asset(id)) {
        thumbs_.erase(id);
        loadMediaCaches(*x);
        scheduleMediaJobs(*x);
    }
    return true;
}

ThumbStripPtr Session::thumbs(Id asset) const {
    auto it = thumbs_.find(asset);
    return it == thumbs_.end() ? nullptr : it->second;
}

WaveformPtr Session::waveform(Id asset, int stream) const {
    auto it = waves_.find({asset, stream});
    return it == waves_.end() ? nullptr : it->second;
}

const ProxyState* Session::proxy(Id asset) const {
    auto it = proxies_.find(asset);
    return it == proxies_.end() ? nullptr : &it->second;
}

void Session::loadMediaCaches(const Asset& a) {
    if (a.info.hasVideo)
        if (auto t = cache_.loadThumbs(a)) thumbs_[a.id] = t;
    for (size_t i = 0; i < a.info.audio.size(); ++i)
        if (!a.info.isImage)
            if (auto w = cache_.loadPeaks(a, int(i))) waves_[{a.id, int(i)}] = w;
    if (a.info.hasVideo && !a.info.isImage && cache_.proxyReady(a)) {
        ProxyState& ps = proxies_[a.id];
        ps.status = ProxyState::Ready;
        ps.path = cache_.proxyPath(a);
        ps.info = probeMedia(ps.path);
        ps.info.origin = 0;  // proxies carry the original media time
        ps.progress = 1;
    }
}

void Session::scheduleMediaJobs(const Asset& a) {
    const std::string key = MediaCache::keyFor(a);
    if (a.info.hasVideo && !thumbs_.count(a.id) && !scheduled_.count(key + "t")) {
        scheduled_.insert(key + "t");
        Asset copy = a;
        jobs_->submit("thumbnails", [this, copy, key](JobToken& tok) {
            ThumbStripPtr s = MediaCache::generateThumbs(copy, tok);
            if (s) cache_.saveThumbs(copy, *s);
            QMetaObject::invokeMethod(this, [this, copy, s, key] {
                scheduled_.erase(key + "t");
                if (s) thumbs_[copy.id] = s;
                emit mediaUpdated();
            });
        }, JobPool::Lane::General, true);  // thumbnails first: they fill the timeline
    }
    if (!a.info.isImage)
        for (size_t i = 0; i < a.info.audio.size(); ++i) {
            if (waves_.count({a.id, int(i)}) || scheduled_.count(key + "a" + std::to_string(i))) continue;
            scheduled_.insert(key + "a" + std::to_string(i));
            auto w = std::make_shared<Waveform>();
            waves_[{a.id, int(i)}] = w;  // filled progressively
            Asset copy = a;
            int stream = int(i);
            jobs_->submit("waveform", [this, copy, stream, w, key](JobToken& tok) {
                bool ok = MediaCache::generatePeaks(copy, stream, *w, tok);
                if (ok) cache_.savePeaks(copy, stream, *w);
                QMetaObject::invokeMethod(this, [this, key, stream] {
                    scheduled_.erase(key + "a" + std::to_string(stream));
                    emit mediaUpdated();
                });
            });
        }
    // proxies for anything above 1080p (long-GOP game recordings scrub slowly at full size)
    QSettings st;
    if (st.value("proxy/auto", true).toBool() && a.info.hasVideo && !a.info.isImage && a.info.video.height > 1080 &&
        !proxies_.count(a.id))
        buildProxy(a.id);
}

void Session::buildProxy(Id assetId) {
    const Asset* a = doc_.project.asset(assetId);
    if (!a || !a->info.hasVideo || a->info.isImage || a->offline) return;
    ProxyState& ps = proxies_[assetId];
    if (ps.status == ProxyState::Queued || ps.status == ProxyState::Building || ps.status == ProxyState::Ready) return;
    ps.status = ProxyState::Queued;
    ps.progress = 0;
    Asset copy = *a;
    std::string out = cache_.proxyPath(copy);
    ps.job = jobs_->submit(
        "proxy",
        [this, copy, out](JobToken& tok) {
            QMetaObject::invokeMethod(this, [this, id = copy.id] {
                proxies_[id].status = ProxyState::Building;
                emit mediaUpdated();
            });
            std::string err;
            bool ok = generateProxy(copy, out, tok, err);
            MediaInfo info;
            if (ok) {
                info = probeMedia(out);
                info.origin = 0;
                ok = info.valid;
            }
            QMetaObject::invokeMethod(this, [this, id = copy.id, ok, err, out, info] {
                ProxyState& p = proxies_[id];
                p.status = ok ? ProxyState::Ready : ProxyState::Failed;
                p.error = err;
                p.path = out;
                p.info = info;
                p.progress = ok ? 1 : 0;
                if (ok) {
                    publish();
                    publishSource();
                    std::set<std::string> keep;
                    for (const auto& [aid, ps] : proxies_)
                        if (!ps.path.empty()) keep.insert(ps.path);
                    jobs_->submit("cache purge", [this, keep](JobToken&) { cache_.purge(keep); });
                }
                emit mediaUpdated();
            });
        },
        JobPool::Lane::Proxy);
}

void Session::setProxiesEnabled(bool on) {
    proxiesEnabled_ = on;
    if (on)
        for (const auto& a : doc_.project.assets)
            if (a.info.hasVideo && !a.info.isImage) buildProxy(a.id);
    publish();
    publishSource();
    emit mediaUpdated();
    emit message(on ? tr("Proxies on (preview only; export always uses the originals)") : tr("Proxies off"), 2500);
}

void Session::setExporting(bool on) {
    exporting_ = on;
    if (jobs_) jobs_->setPaused(on || playing());  // null after shutdown (dialogs closing at exit)
}

// ---------------------------------------------------------------------------------------------
// Collect Files: the project and the media it uses, copied into one folder

std::vector<Id> Session::usedAssets() const {
    std::set<Id> used;
    auto scan = [&](const Sequence& s) {
        for (const auto& c : s.clips)
            if (c.kind == ClipKind::Media) used.insert(c.assetId);
    };
    scan(doc_.project.sequence);
    for (const auto& s : doc_.project.otherSequences) scan(s);
    return {used.begin(), used.end()};
}

std::vector<Id> Session::unusedAssets() const {
    const auto used = usedAssets();
    const std::set<Id> keep(used.begin(), used.end());
    std::vector<Id> out;
    for (const auto& a : doc_.project.assets)
        if (!keep.count(a.id) && a.id != sourceAsset_) out.push_back(a.id);
    return out;
}

bool Session::collectFiles(Project p, const QString& projectName, const QString& destDir, bool onlyUsed, CollectProgress& prog,
                           QString& error) {
    namespace fs = std::filesystem;
    std::set<Id> keep;
    auto scan = [&](const Sequence& s) {
        for (const auto& c : s.clips)
            if (c.kind == ClipKind::Media) keep.insert(c.assetId);
    };
    scan(p.sequence);
    for (const auto& s : p.otherSequences) scan(s);
    std::error_code ec;
    const fs::path root = fs::path(QDir::toNativeSeparators(destDir).toStdWString()).make_preferred();
    const fs::path media = root / L"Media";
    fs::create_directories(media, ec);
    if (ec) {
        error = tr("Cannot create %1").arg(QDir::toNativeSeparators(QString::fromStdWString(media.wstring())));
        return false;
    }
    std::vector<std::pair<fs::path, fs::path>> copies;  // from, to
    std::set<std::wstring> names;
    for (auto& a : p.assets) {
        if (onlyUsed && !keep.count(a.id)) continue;
        const fs::path from = fs::path(QString::fromStdString(a.path).toStdWString());
        std::wstring name = from.filename().wstring();
        for (int i = 2; !names.insert(name).second; ++i)  // same file name from different folders
            name = from.stem().wstring() + L"_" + std::to_wstring(i) + from.extension().wstring();
        const fs::path to = media / name;
        copies.push_back({from, to});
        a.path = QString::fromStdWString(to.wstring()).toStdString();
        a.relPath.clear();
    }
    if (onlyUsed)
        p.assets.erase(std::remove_if(p.assets.begin(), p.assets.end(), [&](const Asset& a) { return !keep.count(a.id); }),
                       p.assets.end());
    uint64_t total = 0;
    for (const auto& [from, to] : copies) total += fs::file_size(from, ec);
    prog.total = total;
    std::vector<char> buf(size_t(8) << 20);
    for (const auto& [from, to] : copies) {
        FILE* in = _wfopen(from.c_str(), L"rb");
        FILE* out = in ? _wfopen(to.c_str(), L"wb") : nullptr;
        if (!in || !out) {
            if (in) std::fclose(in);
            error = tr("Cannot copy %1").arg(QString::fromStdWString(from.wstring()));
            return false;
        }
        bool ok = true;
        while (size_t n = std::fread(buf.data(), 1, buf.size(), in)) {
            if (std::fwrite(buf.data(), 1, n, out) != n) {
                ok = false;
                break;
            }
            prog.done += n;
            if (prog.cancel) {
                ok = false;
                break;
            }
        }
        std::fclose(in);
        if (std::fclose(out) != 0) ok = false;
        if (!ok) {
            fs::remove(to, ec);
            error = prog.cancel ? tr("Cancelled.") : tr("Writing %1 failed (disk full?)").arg(QString::fromStdWString(to.wstring()));
            return false;
        }
    }
    const QString name = projectName.isEmpty() ? tr("Project") : projectName;
    const std::string projPath = QString::fromStdWString((root / (name.toStdWString() + L".cutproj")).wstring()).toStdString();
    std::string err;
    if (!saveProjectFile(p, projPath, err)) {
        error = toQ(err);
        return false;
    }
    prog.projectPath = toQ(projPath);
    return true;
}

} // namespace cut::ui
