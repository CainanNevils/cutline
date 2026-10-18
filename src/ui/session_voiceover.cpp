// Voice-over recording (Sequence > Record Voice-over).
#include "ui/session.h"

#include "audio/recorder.h"
#include "core/edits.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cstdlib>

namespace cut::ui {

namespace {
int64_t usToTicks(int64_t us) { return us * (kTicksPerSecond / 10000) / 100; }  // 35.28 ticks per us

QString voiceoverFolder(const QString& projectPath) {
    QString dir = projectPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                        : QFileInfo(projectPath).absolutePath();
    return QDir(dir).filePath("Voice-over");
}
} // namespace

bool Session::recordingVoiceover() const { return recorder_ && recorder_->recording(); }
float Session::voiceoverPeak() { return recorder_ ? recorder_->peak() : 0.f; }
double Session::voiceoverSeconds() const { return recorder_ ? double(recorder_->frames()) / 48000.0 : 0.0; }

bool Session::startVoiceover(QString& error) {
    if (recordingVoiceover()) return true;
    if (!engine_) {
        error = tr("Playback is not running.");
        return false;
    }
    if (seq().duration() <= 0) {
        error = tr("The sequence is empty - voice-over records while the timeline plays.");
        return false;
    }
    const QString folder = voiceoverFolder(path_);
    if (!QDir().mkpath(folder)) {
        error = tr("Cannot create %1").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    int n = 1;
    QString file;
    do file = QDir(folder).filePath(QString("Voice-over %1.wav").arg(n++, 3, 10, QChar('0')));
    while (QFileInfo::exists(file));

    // Tests and benchmarks never open the microphone: they record what the output device plays.
    const bool loopback = std::getenv("CUTLINE_RECORD_LOOPBACK") != nullptr;
    std::string device;
    if (loopback) {
        if (const char* d = std::getenv("CUTLINE_AUDIO_DEVICE")) device = d;
        else device = QSettings().value("audio/device").toString().toStdString();
    } else device = QSettings().value("audio/input").toString().toStdString();

    if (engine_->isPlaying()) engine_->pause();
    if (!recorder_) recorder_ = std::make_unique<AudioRecorder>();
    std::string err;
    if (!recorder_->start(toS(file), device, loopback, err)) {
        error = toQ(err);
        return false;
    }
    voPath_ = file;
    voStart_ = playhead_ >= seq().duration() - seq().frameDur() ? 0 : playhead_;  // play() restarts at the end
    voTrack_ = targetAudio;
    voOffsets_.clear();
    lastVoiceover_ = 0;
    voTimer_.start(40);
    engine_->play(programViewer_, 1.0);
    emit voiceoverChanged(true);
    emit message(tr("Recording voice-over from %1 - press Space to stop").arg(toQ(recorder_->deviceName())), 0);
    return true;
}

void Session::stopVoiceover(bool keep) {
    if (!recorder_ || !recorder_->recording()) return;
    // last clock samples while still playing, then stop both
    for (int i = 0; i < 3; ++i) {
        int64_t wall = 0;
        Tick t = 0;
        if (engine_ && engine_->clockSample(wall, t)) voOffsets_.push_back(t - usToTicks(wall));
    }
    voTimer_.stop();
    recorder_->stop();
    if (engine_ && engine_->isPlaying()) engine_->pause();
    const int64_t frames = recorder_->frames();
    const int64_t firstUs = recorder_->firstSampleUs();
    emit voiceoverChanged(false);
    if (!keep || frames < 48000 / 10) {  // nothing worth keeping
        QFile::remove(voPath_);
        emit message(keep ? tr("Recording too short - discarded.") : tr("Recording discarded."));
        return;
    }
    // timeline time of the first recorded sample: median of (timeline - wall) offsets
    Tick t0 = voStart_;
    if (!voOffsets_.empty() && firstUs > 0) {
        std::vector<int64_t> o = voOffsets_;
        std::nth_element(o.begin(), o.begin() + o.size() / 2, o.end());
        t0 = usToTicks(firstUs) + o[o.size() / 2];
    }
    const Tick at = std::max(t0, voStart_);  // the head before playback reached the playhead is dropped
    const Tick skip = at - t0;
    const int preferred = voTrack_;
    const QString path = voPath_;
    importFiles({path}, 0, [this, at, skip, preferred](const std::vector<Id>& ids) {
        if (ids.empty()) return;
        const Asset* a = doc_.project.asset(ids.front());
        if (!a || a->info.duration <= skip) return;
        const Asset asset = *a;
        const Tick end = at + (asset.info.duration - skip);
        Id placed = 0;
        edit(tr("Record Voice-over"), [&](Project& p) {
            Sequence& s = p.sequence;
            auto freeTrack = [&](const Track* tr) {
                if (!tr || tr->locked) return false;
                for (const Clip* c : s.clipsOnTrack(tr->id))
                    if (c->start < end && c->end() > at) return false;
                return true;
            };
            const auto audio = s.tracksOf(TrackKind::Audio);
            int idx = -1;
            if (preferred >= 0 && preferred < int(audio.size()) && freeTrack(audio[size_t(preferred)])) idx = preferred;
            for (int i = 0; idx < 0 && i < int(audio.size()); ++i)
                if (freeTrack(audio[size_t(i)])) idx = i;
            if (idx < 0) {
                edit::addTrack(s, TrackKind::Audio);
                idx = int(s.tracksOf(TrackKind::Audio).size()) - 1;
            }
            auto clips = edit::clipsForAsset(s, asset, at, skip, asset.info.duration, 0, idx, false, true);
            if (clips.empty()) return;
            edit::overwriteClips(s, clips);
            placed = clips.front().id;
        });
        lastVoiceover_ = placed;
        if (placed) {
            setSelection({placed});
            emit message(tr("Voice-over placed on the timeline (%1 s).").arg(double(end - at) / kTicksPerSecond, 0, 'f', 1));
        }
    });
}

} // namespace cut::ui
