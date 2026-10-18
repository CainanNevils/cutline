// Keyboard/menu editing commands on the current selection (all undoable).
#include "audio/audio_sync.h"
#include "audio/clip_audio.h"
#include "audio/loudness.h"
#include "core/auto_color.h"
#include "core/edits.h"
#include "core/tracker.h"
#include "core/project_io.h"
#include "engine/frame_renderer.h"
#include "engine/sync_provider.h"
#include "media/frame_reader.h"
#include "media/scene_detect.h"
#include "render/compositor.h"
#include "ui/session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QMetaObject>
#include <QPointF>
#include <QSettings>

#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace cut::ui {

void Session::deleteSelection(bool ripple) {
    if (selTransition_) {
        Id t = selTransition_;
        edit(tr("Delete Transition"), [&](Project& p) { edit::removeTransition(p.sequence, t); });
        return;
    }
    if (!selectedCaptions.empty()) {
        auto ids = selectedCaptions;
        edit(tr("Delete Captions"), [&](Project& p) { edit::deleteCaptions(p.sequence, ids); });
        selectedCaptions.clear();
        return;
    }
    if (selection_.empty()) {
        if (ripple || gapTrack) {
            Id track = gapTrack;
            Tick t = gapTime;
            if (track && t != kInvalidTick)
                edit(tr("Ripple Delete Gap"), [&](Project& p) { edit::rippleDeleteGap(p.sequence, track, t); });
            gapTrack = 0;
        }
        return;
    }
    auto ids = selection_;
    edit(ripple ? tr("Ripple Delete") : tr("Delete"), [&](Project& p) {
        if (ripple) edit::rippleDelete(p.sequence, ids);
        else edit::deleteClips(p.sequence, ids);
    });
    selection_.clear();
    emit selectionChanged();
}

void Session::copySelection() {
    clipboard_.clear();
    clipboardCaptions_.clear();
    Tick origin = INT64_MAX;
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id)) {
            clipboard_.push_back(*c);
            origin = std::min(origin, c->start);
        }
    for (Id id : selectedCaptions)
        for (const auto& c : seq().captions)
            if (c.id == id) {
                clipboardCaptions_.push_back(c);
                origin = std::min(origin, c.start);
            }
    clipboardOrigin_ = origin == INT64_MAX ? 0 : origin;
    if (!clipboard_.empty() || !clipboardCaptions_.empty())
        emit message(tr("Copied %1 item(s)").arg(clipboard_.size() + clipboardCaptions_.size()), 1500);
}

void Session::cutSelection() {
    copySelection();
    deleteSelection(false);
}

void Session::paste(bool insert) {
    if (clipboard_.empty() && clipboardCaptions_.empty()) return;
    const Tick at = playhead_;
    std::vector<Id> newIds;
    Tick end = at;
    edit(insert ? tr("Paste Insert") : tr("Paste"), [&](Project& p) {
        Sequence& s = p.sequence;
        std::vector<Clip> clips;
        std::map<Id, Id> linkMap;
        for (Clip c : clipboard_) {
            if (!s.track(c.trackId)) continue;
            c.id = s.newId();
            if (c.linkId) {
                auto it = linkMap.find(c.linkId);
                if (it == linkMap.end()) it = linkMap.emplace(c.linkId, s.newId()).first;
                c.linkId = it->second;
            }
            c.start = c.start - clipboardOrigin_ + at;
            end = std::max(end, c.end());
            clips.push_back(c);
        }
        newIds = insert ? edit::insertClips(s, clips, at) : edit::overwriteClips(s, clips);
        for (Caption c : clipboardCaptions_) edit::addCaption(s, c.start - clipboardOrigin_ + at, c.end - clipboardOrigin_ + at, c.text);
    });
    setSelection(newIds, false);
    setPlayhead(end);
}

void Session::addEditAtPlayhead(bool allTracks) {
    const Tick t = playhead_;
    std::vector<Id> only;
    if (!allTracks)
        for (Id id : selection_)
            if (const Clip* c = seq().clip(id); c && c->contains(t)) only.push_back(id);
    edit(tr("Add Edit"), [&](Project& p) { edit::addEdit(p.sequence, t, only); });
}

void Session::toggleLink() {
    if (selection_.empty()) return;
    bool anyLinked = false;
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id); c && c->linkId) anyLinked = true;
    auto ids = selection_;
    if (anyLinked) edit(tr("Unlink"), [&](Project& p) { edit::unlinkClips(p.sequence, ids); });
    else edit(tr("Link"), [&](Project& p) { edit::linkClips(p.sequence, ids); });
}

void Session::toggleEnable() {
    if (selection_.empty()) return;
    bool anyEnabled = false;
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id); c && c->enabled) anyEnabled = true;
    auto ids = selection_;
    edit(anyEnabled ? tr("Disable") : tr("Enable"), [&](Project& p) { edit::setEnabled(p.sequence, ids, !anyEnabled); });
}

void Session::addFrameHold(bool segment) {
    const Tick t = playhead_;
    Id target = 0;
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id)) {
            const Track* tr = seq().track(c->trackId);
            if (tr && tr->kind == TrackKind::Video && !tr->locked && c->contains(t) && c->kind == ClipKind::Media) target = id;
        }
    if (!target) {
        // topmost video clip under the playhead
        auto vts = seq().tracksOf(TrackKind::Video);
        for (auto it = vts.rbegin(); it != vts.rend() && !target; ++it)
            if (!(*it)->locked)
                for (const Clip* c : seq().clipsOnTrack((*it)->id))
                    if (c->contains(t) && c->kind == ClipKind::Media) target = c->id;
    }
    if (!target) {
        emit message(tr("Place the playhead over a video clip to add a frame hold."));
        return;
    }
    Id hold = 0;
    edit(segment ? tr("Insert Frame Hold Segment") : tr("Add Frame Hold"), [&](Project& p) {
        hold = segment ? edit::insertFrameHoldSegment(p.sequence, target, t, 2 * kTicksPerSecond)
                       : edit::addFrameHold(p.sequence, target, t);
    });
    if (hold) setSelection({hold}, false);
}

void Session::addMarkerAtPlayhead() {
    const Tick t = playhead_;
    edit(tr("Add Marker"), [&](Project& p) { edit::addMarker(p.sequence, t); });
}

void Session::gotoEdit(int dir) {
    auto pts = edit::editPoints(seq());
    if (!seq().markers.empty())
        for (const auto& m : seq().markers) pts.push_back(m.t);
    std::sort(pts.begin(), pts.end());
    Tick best = playhead_;
    if (dir > 0) {
        auto it = std::upper_bound(pts.begin(), pts.end(), playhead_);
        if (it != pts.end()) best = *it;
    } else {
        auto it = std::lower_bound(pts.begin(), pts.end(), playhead_);
        if (it != pts.begin()) best = *(it - 1);
    }
    setPlayhead(best);
}

void Session::applyDefaultTransition(bool audio, int type) {
    // at the edit point nearest the playhead on the targeted (or selected clip's) track
    const Tick t = playhead_;
    const Sequence& s = seq();
    Id trackId = 0;
    if (const Clip* c = primaryClip() ? s.clip(primaryClip()) : nullptr) {
        const Track* tr = s.track(c->trackId);
        if (tr && (tr->kind == TrackKind::Audio) == audio) trackId = tr->id;
    }
    if (!trackId) {
        const Track* tr = s.trackAt(audio ? TrackKind::Audio : TrackKind::Video, audio ? targetAudio : targetVideo);
        if (tr) trackId = tr->id;
    }
    if (!trackId) return;
    const Clip* a = nullptr;
    const Clip* b = nullptr;
    Tick bestD = INT64_MAX;
    auto clips = s.clipsOnTrack(trackId);
    for (size_t i = 0; i < clips.size(); ++i) {
        Tick cut = clips[i]->start;
        Tick d = std::llabs(cut - t);
        if (d < bestD) {
            bestD = d;
            b = clips[i];
            a = (i > 0 && clips[i - 1]->end() == cut) ? clips[i - 1] : nullptr;
        }
        Tick e = clips[i]->end();
        if (std::llabs(e - t) < bestD && !(i + 1 < clips.size() && clips[i + 1]->start == e)) {
            bestD = std::llabs(e - t);
            a = clips[i];
            b = nullptr;
        }
    }
    if (!a && !b) return;
    Id ida = a ? a->id : 0, idb = b ? b->id : 0;
    edit(audio ? tr("Apply Audio Transition") : tr("Apply Video Transition"), [&](Project& p) {
        edit::addTransition(p.sequence, trackId, ida, idb,
                            audio ? TransitionType::ConstantPower
                                  : (type >= 0 ? TransitionType(type) : TransitionType::CrossDissolve),
                            kTicksPerSecond,
                            TransitionAlign::Center);
    });
}

void Session::selectAll() {
    std::vector<Id> ids;
    for (const auto& c : seq().clips) {
        const Track* t = seq().track(c.trackId);
        if (t && !t->locked) ids.push_back(c.id);
    }
    setSelection(ids, false);
}

void Session::addCaptionAtPlayhead() {
    Tick t = playhead_;
    // inside an existing caption: the new one follows it instead of overlapping
    for (const auto& c : seq().captions)
        if (c.start <= t && t < c.end) t = c.end;
    Id id = 0;
    edit(tr("Add Caption"), [&](Project& p) {
        Tick end = t + 3 * kTicksPerSecond;
        for (const auto& c : p.sequence.captions)
            if (c.start > t) end = std::min(end, c.start);
        if (end - t < p.sequence.frameDur()) end = t + p.sequence.frameDur();
        id = edit::addCaption(p.sequence, t, end, "New caption");
    });
    selectedCaptions = {id};
    emit selectionChanged();
}

void Session::nudge(int frames) {
    if (selection_.empty()) return;
    auto ids = selection_;
    const Tick dt = seq().frameDur() * frames;
    edit(tr("Nudge"), [&](Project& p) { edit::moveClips(p.sequence, ids, dt, 0, 0); });
}

void Session::setClipFit(FitMode m) {
    auto ids = selection_;
    edit(tr("Scale to Frame"), [&](Project& p) {
        for (Id id : ids)
            if (Clip* c = p.sequence.clip(id)) {
                c->fit = m;
                if (m != FitMode::None) c->motion.params["scale"] = Param{100, {}};
            }
    });
}

const Clip* Session::clipAtPlayhead() const {
    const Sequence& sq = seq();
    const Tick t = playhead_;
    auto onTrack = [&](const Track* tr) -> const Clip* {
        if (!tr) return nullptr;
        for (const auto& c : sq.clips)
            if (c.trackId == tr->id && c.contains(t)) return &c;
        return nullptr;
    };
    // the targeted video track first, then the topmost video track with a clip, then audio
    if (const Clip* c = onTrack(sq.trackAt(TrackKind::Video, targetVideo))) return c;
    auto vts = sq.tracksOf(TrackKind::Video);
    for (auto it = vts.rbegin(); it != vts.rend(); ++it)
        if (const Clip* c = onTrack(*it)) return c;
    if (const Clip* c = onTrack(sq.trackAt(TrackKind::Audio, targetAudio))) return c;
    for (const Track* tr : sq.tracksOf(TrackKind::Audio))
        if (const Clip* c = onTrack(tr)) return c;
    return nullptr;
}

void Session::markClipAtPlayhead() {
    const Clip* c = clipAtPlayhead();
    if (!c) {
        emit message(tr("No clip under the playhead."));
        return;
    }
    const Tick a = c->start, b = c->end();
    edit(tr("Mark Clip"), [&](Project& p) {
        p.sequence.inPoint = a;
        p.sequence.outPoint = b;
    });
}

void Session::selectClipAtPlayhead() {
    if (const Clip* c = clipAtPlayhead()) setSelection({c->id});
}
// ---------------------------------------------------------------------------------------------
// Remove Silence: pauses in an audio clip's waveform, cut on every track

Id Session::silenceSourceClip() const {
    // the selected audio clip, or the first audio partner of a selected video clip
    for (Id id : selection_)
        for (Id l : seq().linkedClips(id))
            if (const Clip* c = seq().clip(l); c && c->kind == ClipKind::Media && seq().track(c->trackId)->kind == TrackKind::Audio)
                return l;
    return 0;
}

std::vector<TickRange> Session::findSilences(Id clipId, double thresholdDb, Tick minLength, Tick pad, QString& why) const {
    const Clip* c = seq().clip(clipId);
    if (!c) {
        why = tr("Select an audio clip (or a clip with linked audio).");
        return {};
    }
    if (c->isHold() || c->speed != 1.0 || c->reverse) {
        why = tr("Remove Silence works on clips at normal speed.");
        return {};
    }
    WaveformPtr wf = waveform(c->assetId, c->stream);
    if (!wf || !wf->complete.load()) {
        why = tr("The clip's waveform is still being analysed - try again in a moment.");
        return {};
    }
    const Tick win = kTicksPerSecond / 100;  // 10 ms
    std::vector<float> levels;
    levels.reserve(size_t(c->duration / win) + 1);
    for (Tick t = 0; t < c->duration; t += win) {
        const int64_t s0 = ticksToSamples(c->sourceIn + t, 48000);
        float mn = 0, mx = 0;
        levels.push_back(wf->range(s0, s0 + 480, mn, mx) ? std::max(std::abs(mn), std::abs(mx)) : 0.f);
    }
    return edit::findSilences(levels, c->start, win, thresholdDb, minLength, pad, seq().frameDur());
}

int Session::removeSilence(Id clipId, double thresholdDb, Tick minLength, Tick pad) {
    QString why;
    auto spans = findSilences(clipId, thresholdDb, minLength, pad, why);
    if (spans.empty()) {
        emit message(why.isEmpty() ? tr("No pauses found with these settings.") : why);
        return 0;
    }
    Tick total = 0;
    for (const auto& r : spans) total += r.b - r.a;
    edit(tr("Remove Silence"), [&](Project& p) { edit::removeSpans(p.sequence, spans); });
    emit message(tr("Removed %n pause(s), %1 s in total.", "", int(spans.size())).arg(ticksToSeconds(total), 0, 'f', 1));
    return int(spans.size());
}
int Session::addBeatMarkers() {
    const Id id = silenceSourceClip();
    const Clip* c = seq().clip(id);
    if (!c || c->speed != 1.0 || c->reverse || c->isHold()) {
        emit message(tr("Select a music clip (audio at normal speed) to mark its beats."));
        return 0;
    }
    WaveformPtr wf = waveform(c->assetId, c->stream);
    if (!wf || !wf->complete.load()) {
        emit message(tr("The clip's waveform is still being analysed - try again in a moment."));
        return 0;
    }
    const Tick win = kTicksPerSecond / 100;
    std::vector<float> levels;
    for (Tick t = 0; t < c->duration; t += win) {
        const int64_t s0 = ticksToSamples(c->sourceIn + t, 48000);
        float mn = 0, mx = 0;
        levels.push_back(wf->range(s0, s0 + 480, mn, mx) ? std::max(std::abs(mn), std::abs(mx)) : 0.f);
    }
    const auto beats = edit::findBeats(levels, c->start, win, kTicksPerSecond / 4, seq().frameDur());
    if (beats.empty()) {
        emit message(tr("No clear beats found in that clip."));
        return 0;
    }
    edit(tr("Markers at Beats"), [&](Project& p) {
        for (Tick t : beats) {
            bool exists = false;
            for (const auto& m : p.sequence.markers) exists |= m.t == t;
            if (exists) continue;
            const Id mid = edit::addMarker(p.sequence, t);
            for (auto& m : p.sequence.markers)
                if (m.id == mid) m.color = 0xFFE8C547;  // yellow: beat markers
        }
    });
    emit message(tr("Added %n beat marker(s) - cuts snap to them.", "", int(beats.size())));
    return int(beats.size());
}

int Session::addHighlightMarkers() {
    const Id id = silenceSourceClip();
    const Clip* c = seq().clip(id);
    if (!c || c->speed != 1.0 || c->reverse || c->isHold()) {
        emit message(tr("Select a gameplay clip (with its audio, at normal speed) to find its loudest moments."));
        return 0;
    }
    WaveformPtr wf = waveform(c->assetId, c->stream);
    if (!wf || !wf->complete.load()) {
        emit message(tr("The clip's waveform is still being analysed - try again in a moment."));
        return 0;
    }
    const Tick win = kTicksPerSecond / 100;
    std::vector<float> levels;
    for (Tick t = 0; t < c->duration; t += win) {
        const int64_t s0 = ticksToSamples(c->sourceIn + t, 48000);
        float mn = 0, mx = 0;
        levels.push_back(wf->range(s0, s0 + 480, mn, mx) ? std::max(std::abs(mn), std::abs(mx)) : 0.f);
    }
    // about one per minute, at least 10 s apart
    const int maxCount = std::clamp(int(c->duration / (60 * kTicksPerSecond)) + 3, 3, 30);
    const auto spots = edit::findHighlights(levels, c->start, win, maxCount, 10 * kTicksPerSecond, seq().frameDur());
    if (spots.empty()) {
        emit message(tr("No moments stand out from the rest of that clip."));
        return 0;
    }
    int added = 0;
    edit(tr("Markers at Highlights"), [&](Project& p) {
        for (Tick t : spots) {
            bool exists = false;
            for (const auto& m : p.sequence.markers) exists |= m.t == t;
            if (exists) continue;
            const Id mid = edit::addMarker(p.sequence, t, toS(tr("Highlight %1").arg(++added)));
            for (auto& m : p.sequence.markers)
                if (m.id == mid) m.color = 0xFFE5484D;  // red: highlights
        }
    });
    emit message(tr("Marked %n loud moment(s) - Shift+M jumps between markers.", "", added));
    return added;
}

int Session::addEditsAtMarkers() {
    const Sequence& q = seq();
    const Tick a = q.inPoint != kInvalidTick ? q.inPoint : 0;
    const Tick b = q.outPoint != kInvalidTick ? q.outPoint : q.duration();
    std::vector<Tick> at;
    for (const auto& m : q.markers)
        if (m.t > a && m.t < b) at.push_back(snapToFrame(m.t));
    if (at.empty()) {
        emit message(tr("No markers to cut at (add some with M, or Markers > Add Markers at Beats)."));
        return 0;
    }
    edit(tr("Add Edits at Markers"), [&](Project& p) {
        for (Tick t : at) edit::addEdit(p.sequence, t, {});
    });
    emit message(tr("Cut at %n marker(s).", "", int(at.size())));
    return int(at.size());
}

int Session::enhanceVoice() {
    std::vector<Id> ids;
    for (Id id : selection_)
        for (Id l : seq().linkedClips(id))
            if (const Clip* c = seq().clip(l); c && c->kind == ClipKind::Media && seq().track(c->trackId)->kind == TrackKind::Audio)
                if (std::find(ids.begin(), ids.end(), l) == ids.end()) ids.push_back(l);
    if (ids.empty()) {
        emit message(tr("Select a microphone clip first."));
        return 0;
    }
    // rumble cut, steady noise down, a little presence, then even out the level
    Effect hp = makeEffect(fx::HighPass);
    hp.params["frequency"].value = 80;
    Effect dn = makeEffect(fx::Denoise);
    dn.params["reduction"].value = 12;
    Effect eq = makeEffect(fx::Eq);
    eq.params["low"].value = -2;
    eq.params["mid"].value = 3;
    eq.params["midFreq"].value = 3000;
    eq.params["high"].value = 1.5;
    Effect comp = makeEffect(fx::Compressor);
    comp.params["threshold"].value = -22;
    comp.params["ratio"].value = 3;
    comp.params["attack"].value = 5;
    comp.params["release"].value = 120;
    comp.params["makeup"].value = 4;
    edit(tr("Enhance Voice"), [&](Project& p) {
        for (Id id : ids)
            if (Clip* c = p.sequence.clip(id)) {
                std::erase_if(c->effects, [](const Effect& e) {
                    return e.type == fx::HighPass || e.type == fx::Denoise || e.type == fx::Eq || e.type == fx::Compressor;
                });
                for (const Effect* e : {&hp, &dn, &eq, &comp}) c->effects.push_back(*e);
            }
    });
    emit message(tr("Voice chain added to %n clip(s): High-Pass, DeNoise, EQ, Compressor (adjust in Effect Controls).", "",
                    int(ids.size())));
    return int(ids.size());
}
void Session::detectScenes(bool cut) {
    // the selected (or linked) video clip, analysed in the background
    Id vid = 0;
    for (Id id : selection_)
        for (Id l : seq().linkedClips(id))
            if (const Clip* c = seq().clip(l); c && c->kind == ClipKind::Media && seq().track(c->trackId)->kind == TrackKind::Video)
                vid = l;
    const Clip* c = seq().clip(vid);
    const Asset* a = c ? project().asset(c->assetId) : nullptr;
    if (!c || !a || c->isHold()) {
        emit message(tr("Select a video clip to detect its scene changes."));
        return;
    }
    const Clip clip = *c;
    const std::string path = a->path;
    const MediaInfo info = a->info;
    const Tick from = std::min(clip.sourceIn, clip.sourceOut()), to = std::max(clip.sourceIn, clip.sourceOut());
    emit message(tr("Detecting scene changes..."), 3000);
    jobs_->submit("scenes", [this, clip, path, info, from, to, cut](JobToken& tok) {
        std::vector<Tick> media;
        auto progress = [this](double f) {
            QMetaObject::invokeMethod(this, [this, f] { emit message(tr("Detecting scene changes... %1%").arg(int(f * 100)), 2000); });
        };
        if (!cut::detectScenes(path, info, from, to, SceneOptions{}, media, progress, [&] { return !tok.checkpoint(); })) return;
        QMetaObject::invokeMethod(this, [this, clip, media, cut] { applyScenes(clip, media, cut); });
    });
}

void Session::applyScenes(const Clip& clip, const std::vector<Tick>& media, bool cut) {
    // media times -> timeline times of the clip as it was analysed (it may have moved: use its id)
    const Clip* now = seq().clip(clip.id);
    if (!now) return;
    std::vector<Tick> times;
    const double sp = now->speed > 0 ? now->speed : 1.0;
    for (Tick m : media) {
        const Tick local = now->reverse ? Tick(double(now->sourceOut() - m) / sp) : Tick(double(m - now->sourceIn) / sp);
        const Tick t = snapToFrame(now->start + local);
        if (t > now->start && t < now->end()) times.push_back(t);
    }
    if (times.empty()) {
        emit message(tr("No scene changes found in that clip."));
        return;
    }
    const Id track = now->trackId, asset = now->assetId;
    edit(cut ? tr("Cut at Scene Changes") : tr("Scene Markers"), [&](Project& p) {
        for (Tick t : times) {
            if (!cut) {
                const Id mid = edit::addMarker(p.sequence, t);
                for (auto& m : p.sequence.markers)
                    if (m.id == mid) m.color = 0xFF39C5CF;  // cyan: scene markers
                continue;
            }
            for (const auto& x : p.sequence.clips)  // the piece of the clip that contains t
                if (x.trackId == track && x.assetId == asset && x.start < t && t < x.end()) {
                    edit::razor(p.sequence, x.id, t);
                    break;
                }
        }
    });
    emit message(cut ? tr("Cut at %n scene change(s).", "", int(times.size())) : tr("Added %n scene marker(s).", "", int(times.size())));
}
// ---------------------------------------------------------------------------------------------
// Sequences (tabs). The open one lives in Project::sequence; switching is an undoable step.

std::vector<Id> Session::sequenceUids() const {
    std::vector<Id> uids;
    for (const auto& s : project().otherSequences) uids.push_back(s.uid);
    uids.insert(uids.begin() + std::clamp(project().activeSequence, 0, int(uids.size())), seq().uid);
    return uids;
}

QStringList Session::sequenceNames() const {
    QStringList names;
    for (const auto& s : project().otherSequences) names << toQ(s.name);
    names.insert(std::clamp(project().activeSequence, 0, int(names.size())), toQ(seq().name));
    return names;
}

namespace {
QString uniqueName(const QStringList& names, const QString& base) {
    if (!names.contains(base)) return base;
    for (int i = 2;; ++i)
        if (!names.contains(QString("%1 %2").arg(base).arg(i))) return QString("%1 %2").arg(base).arg(i);
}
} // namespace

void Session::switchSequence(int tab) {
    if (tab == project().activeSequence || tab < 0 || tab > int(project().otherSequences.size())) return;
    stop();
    seqPlayheads_[project().activeSequence] = playhead_;
    edit(tr("Switch Sequence"), [&](Project& p) { edit::switchSequence(p, tab); });
    setSelection({}, false);
    setPlayhead(seqPlayheads_.count(tab) ? seqPlayheads_[tab] : 0);
    emit projectLoaded();  // panels refit (timeline zoom) as for a newly opened sequence
}

bool Session::speedRampAtPlayhead(double slow, Tick slowSource, Tick rampSource) {
    const Tick t = snapToFrame(playhead_);
    Id target = 0;
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id); c && c->kind == ClipKind::Media && c->contains(t) &&
                                            seq().track(c->trackId)->kind == TrackKind::Video)
            target = id;
    if (!target)
        if (const Clip* c = clipAtPlayhead(); c && c->kind == ClipKind::Media) target = c->id;
    bool ok = false;
    if (target)
        edit(tr("Speed Ramp"), [&](Project& p) { ok = edit::speedRamp(p.sequence, p.assets, target, t, slow, slowSource, rampSource); });
    if (!ok)
        emit message(tr("Put the playhead in the middle of a clip at 100 % speed with room for the ramp on both sides."));
    return ok;
}

QStringList Session::animationNames() {
    return {tr("Fade In"), tr("Fade Out"), tr("Slide Up In"), tr("Slide In from Left"), tr("Slide In from Right"),
            tr("Pop In"), tr("Slow Zoom (Ken Burns)"), tr("Slide Down Out"), tr("Typewriter (titles)")};
}

bool Session::insertInstantReplay(double seconds, double speed) {
    stop();
    TickRange r;
    edit(tr("Instant Replay"), [&](Project& p) {
        Sequence& s = p.sequence;
        r = edit::instantReplay(s, playhead_, secondsToTicks(seconds), speed);
        if (r.len() <= 0) return;
        // REPLAY title on the lowest free video track above everything in the replay
        Id target = 0;
        auto vts = s.tracksOf(TrackKind::Video);
        for (auto it = vts.rbegin(); it != vts.rend(); ++it) {
            bool free = true;
            for (const auto* c : s.clipsOnTrack((*it)->id))
                if (c->start < r.b && c->end() > r.a) free = false;
            if (free && !(*it)->locked) target = (*it)->id;
            else if (target) break;
        }
        if (!target) target = edit::addTrack(s, TrackKind::Video);
        const int H = s.settings.height;
        Clip t = makeTextClip(target, r.a, r.len(), "REPLAY");
        t.name = "Replay";
        t.textStyle.size = std::max(24.0, H * 0.055);
        t.textStyle.weight = 800;
        t.textStyle.italic = true;
        t.textStyle.tracking = 12;
        t.textStyle.box = true;
        t.textStyle.boxColor = 0xFFE5484D;
        t.textStyle.boxOpacity = 100;
        t.textStyle.boxPadding = H * 0.012;
        t.textStyle.boxRadius = H * 0.006;
        t.motion.params["posY"].value = std::round(-H * 0.38);
        edit::applyAnimation(t, edit::Animation::PopIn, s.frameDur(), s.settings.width, H);
        edit::overwriteClips(s, {t});
    });
    if (r.len() <= 0) {
        emit message(tr("Nothing before the playhead to replay."));
        return false;
    }
    setPlayhead(r.a);
    emit message(tr("Instant replay: %1 s at %2 % - the REPLAY title and speed stay editable.")
                     .arg(seconds, 0, 'f', 0)
                     .arg(int(std::lround(speed * 100))), 6000);
    return true;
}

int Session::fitMusicToEnd() {
    const Id id = silenceSourceClip();  // the selected audio clip (or the audio linked to it)
    const Clip* c = seq().clip(id);
    if (!c) {
        emit message(tr("Select the music clip first."));
        return 0;
    }
    Tick end = 0;  // where the picture ends
    for (const auto& x : seq().clips)
        if (const Track* t = seq().track(x.trackId); t && t->kind == TrackKind::Video) end = std::max(end, x.end());
    if (end == 0) end = seq().duration();
    if (end <= c->start) {
        emit message(tr("The music starts after the video ends."));
        return 0;
    }
    int pieces = 0;
    edit(tr("Fit Music to End"), [&](Project& p) {
        Sequence& s = p.sequence;
        // music linked to video would leave its partner behind: it goes alone
        if (Clip* m = s.clip(id); m && m->linkId) edit::unlinkClips(s, {id});
        pieces = edit::fitAudioToEnd(s, p.assets, id, end, 3 * kTicksPerSecond);
    });
    if (!pieces) emit message(tr("That clip cannot be fitted (audio at normal speed on an unlocked track)."));
    else if (pieces == 1) emit message(tr("Music ends with the video, fading out over 3 s."));
    else emit message(tr("Music looped %1 times with crossfades to end with the video.").arg(pieces - 1));
    return pieces;
}

bool Session::punchIn(Id clipId, double x, double y, double factor) {
    const Clip* c = seq().clip(clipId);
    if (!c || !c->contains(playhead_)) {
        emit message(tr("Put the playhead on the clip to zoom into."));
        return false;
    }
    bool ok = false;
    edit(tr("Punch In"), [&](Project& p) {
        if (Clip* m = p.sequence.clip(clipId))
            ok = edit::punchIn(*m, playhead_ - m->start, x, y, factor, kTicksPerSecond * 3 / 10, 2 * kTicksPerSecond,
                               p.sequence.frameDur(), p.sequence.settings.width, p.sequence.settings.height);
    });
    if (!ok) emit message(tr("Not enough of the clip left after the playhead for a punch-in."));
    return ok;
}

QStringList Session::titleTemplateNames() { return {tr("Lower Third"), tr("Subscribe Reminder"), tr("Countdown 3-2-1")}; }

std::vector<Id> Session::addTitleTemplate(int which) {
    const Tick t0 = playhead_;
    const Tick dur = which == 0 ? 5 * kTicksPerSecond : which == 1 ? 4 * kTicksPerSecond : 3 * kTicksPerSecond;
    std::vector<Id> ids;
    edit(titleTemplateNames().value(which), [&](Project& p) {
        Sequence& s = p.sequence;
        const int W = s.settings.width, H = s.settings.height;
        const Tick fd = s.frameDur();
        // the lowest free video track above everything in [t0, t0 + dur)
        Id target = 0;
        auto vts = s.tracksOf(TrackKind::Video);
        for (auto it = vts.rbegin(); it != vts.rend(); ++it) {
            bool free = true;
            for (const auto* c : s.clipsOnTrack((*it)->id))
                if (c->start < t0 + dur && c->end() > t0) free = false;
            if (free && !(*it)->locked) target = (*it)->id;
            else if (target) break;
        }
        if (!target) target = edit::addTrack(s, TrackKind::Video);
        auto widthOf = [](const TextStyle& st, const QString& text) {  // as the rasterizer lays it out
            QFont f(toQ(st.font));
            f.setPixelSize(std::max(1, int(std::lround(st.size))));
            f.setWeight(QFont::Weight(std::clamp(st.weight, 100, 900)));
            f.setItalic(st.italic);
            if (st.tracking != 0) f.setLetterSpacing(QFont::AbsoluteSpacing, st.size * st.tracking / 100.0);
            QFontMetricsF fm(f);
            double w = 0;
            for (const QString& l : text.split('\n')) w = std::max(w, fm.horizontalAdvance(l));
            return w;
        };
        auto place = [&](Clip c, std::initializer_list<edit::Animation> anims) {
            for (auto a : anims) edit::applyAnimation(c, a, fd, W, H);
            ids.push_back(edit::overwriteClips(s, {c}).front());
        };
        const double margin = 0.06 * H;
        if (which == 0) {
            Clip c = makeTextClip(target, t0, dur, "Your Name\n@yourchannel");
            c.name = "Lower Third";
            TextStyle& st = c.textStyle;
            st.size = H * 0.042;
            st.align = 0;
            st.shadow = false;
            st.box = true;
            st.boxColor = 0xFF111318;
            st.boxOpacity = 85;
            st.boxPadding = H * 0.016;
            st.boxRadius = H * 0.008;
            st.lineSpacing = 1.1;
            const double w = widthOf(st, toQ(c.text)), h = st.size * 2.4;
            c.motion.params["posX"].value = std::round(-W / 2.0 + margin + st.boxPadding + w / 2);
            c.motion.params["posY"].value = std::round(H / 2.0 - margin - st.boxPadding - h / 2);
            place(c, {edit::Animation::SlideLeftIn, edit::Animation::FadeOut});
        } else if (which == 1) {
            Clip c = makeTextClip(target, t0, dur, "SUBSCRIBE");
            c.name = "Subscribe";
            TextStyle& st = c.textStyle;
            st.size = H * 0.045;
            st.weight = 900;
            st.tracking = 6;
            st.shadow = false;
            st.box = true;
            st.boxColor = 0xFFE5484D;
            st.boxOpacity = 100;
            st.boxPadding = H * 0.018;
            st.boxRadius = H * 0.012;
            const double w = widthOf(st, toQ(c.text));
            c.motion.params["posX"].value = std::round(W / 2.0 - margin - st.boxPadding - w / 2);
            c.motion.params["posY"].value = std::round(H / 2.0 - margin - st.boxPadding - st.size * 0.6);
            place(c, {edit::Animation::PopIn, edit::Animation::FadeOut});
        } else {
            for (int k = 0; k < 3; ++k) {
                Clip c = makeTextClip(target, t0 + k * kTicksPerSecond, kTicksPerSecond, std::to_string(3 - k));
                c.name = "Countdown";
                TextStyle& st = c.textStyle;
                st.size = H * 0.3;
                st.weight = 900;
                st.stroke = true;
                st.strokeWidth = H * 0.006;
                st.shadowBlur = H * 0.01;
                place(c, {edit::Animation::PopIn, edit::Animation::FadeOut});
            }
        }
    });
    if (!ids.empty()) setSelection({ids.front()});
    return ids;
}

int Session::replaceWithSourceClip() {
    const Asset* a = project().asset(sourceAsset_);
    if (!a) {
        emit message(tr("Open the new clip in the Source monitor first (and set its In point)."));
        return 0;
    }
    if (selection_.empty()) {
        emit message(tr("Select the clips to replace."));
        return 0;
    }
    const Asset asset = *a;
    const Tick in = sourceIn != kInvalidTick ? sourceIn : 0;
    int n = 0;
    edit(tr("Replace With Clip"), [&](Project& p) { n = edit::replaceMedia(p.sequence, asset, selection_, in); });
    if (n) emit message(tr("Replaced %n clip(s) with %1.", "", n).arg(toQ(asset.name)));
    else emit message(tr("Nothing to replace there (locked, reversed, or past the end of the new media)."));
    return n;
}

bool Session::canTrackMask(Id clipId) const {
    const Clip* c = seq().clip(clipId);
    if (!c || c->kind != ClipKind::Adjustment) return false;
    for (const auto& e : c->effects)
        if (e.type == fx::Mask && e.enabled) return true;
    return false;
}

int Session::trackMaskForward(Id clipId, const std::function<bool(double)>& progress) {
    if (!canTrackMask(clipId)) {
        emit message(tr("Select a Blur or Mosaic Region (an adjustment layer with a Shape Mask)."));
        return 0;
    }
    const Clip clip = *seq().clip(clipId);
    if (!clip.contains(playhead_)) {
        emit message(tr("Put the playhead on the region where tracking should start."));
        return 0;
    }
    stop();
    std::string err;
    auto gpu = GpuDevice::create(false, err);
    Compositor comp;
    if (!gpu || !comp.init(gpu, err)) {
        emit message(toQ(err));
        return 0;
    }
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, true);
    // the picture under the region: everything but the region itself
    RenderSnapshot snap = *snapshot(true);
    std::erase_if(snap.seq.clips, [&](const Clip& c) { return c.id == clipId; });
    snap.showCaptions = false;
    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
    const int aw = 480, ah = std::max(16, int(std::lround(480.0 * H / std::max(1, W)))) & ~1;
    auto canvas = comp.makeCanvas(aw, ah, false);
    if (!canvas) return 0;
    const Effect* mask = nullptr;
    for (const auto& e : clip.effects)
        if (e.type == fx::Mask && e.enabled) mask = &e;
    const Tick fd = seq().frameDur();
    const Tick step = seq().settings.fps.toDouble() > 40 ? 2 * fd : fd;  // every other frame at 50/60 fps
    const Tick t0 = snapToFrame(playhead_);
    const Tick tEnd = std::min(clip.end() - fd, t0 + 120 * kTicksPerSecond);
    std::vector<uint8_t> px, luma(size_t(aw) * ah);
    auto frameLuma = [&](Tick t) -> bool {
        renderer.render(snap, t, *canvas, provider, true);
        int pitch = 0;
        if (!comp.readback(*canvas, px, pitch)) return false;
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < aw; ++x) {
                const uint8_t* p = px.data() + size_t(y) * pitch + size_t(x) * 4;
                luma[size_t(y) * aw + x] = uint8_t((54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8);
            }
        return true;
    };
    const Tick l0 = t0 - clip.start;
    RegionTracker tracker;
    if (!frameLuma(t0) || !tracker.start(luma.data(), aw, ah, mask->p("cx", l0) / 100 * aw, mask->p("cy", l0) / 100 * ah,
                                         std::max(4.0, mask->p("w", l0) / 100 * aw), std::max(4.0, mask->p("h", l0) / 100 * ah))) {
        emit message(tr("Nothing to follow under the region there (a flat area?)."));
        return 0;
    }
    std::vector<std::tuple<Tick, double, double>> keys{{l0, tracker.x() / aw * 100, tracker.y() / ah * 100}};
    bool lost = false;
    for (Tick t = t0 + step; t <= tEnd; t += step) {
        if (progress && !progress(double(t - t0) / double(std::max<Tick>(1, tEnd - t0)))) break;
        if (!frameLuma(t)) break;
        if (!tracker.track(luma.data(), aw, ah)) {
            lost = true;
            break;
        }
        keys.emplace_back(t - clip.start, tracker.x() / aw * 100, tracker.y() / ah * 100);
    }
    if (keys.size() < 2) {
        emit message(tr("Lost the region right away - try a bigger mask over something more distinctive."));
        return 0;
    }
    const Tick from = std::get<0>(keys.front()), to = std::get<0>(keys.back());
    edit(tr("Track Mask"), [&](Project& p) {
        Clip* c = p.sequence.clip(clipId);
        if (!c) return;
        for (auto& e : c->effects) {
            if (e.type != fx::Mask || !e.enabled) continue;
            for (const char* name : {"cx", "cy"}) {
                Param& prm = e.params[name];
                std::erase_if(prm.keys, [&](const Keyframe& k) { return k.t >= from && k.t <= to; });
            }
            for (const auto& [t, x, y] : keys) {
                e.params["cx"].addKey(t, std::round(x * 100) / 100);
                e.params["cy"].addKey(t, std::round(y * 100) / 100);
            }
            break;
        }
    });
    emit message(lost ? tr("Tracked %1 s, then lost the region - move the mask there and track again.").arg(ticksToSeconds(to - from), 0, 'f', 1)
                      : tr("Tracked %1 s.").arg(ticksToSeconds(to - from), 0, 'f', 1),
                 8000);
    return int(keys.size());
}

int Session::stabilizeClip(Id clipId, double smoothSeconds, const std::function<bool(double)>& progress) {
    const Clip* cp = seq().clip(clipId);
    const Track* trk = cp ? seq().track(cp->trackId) : nullptr;
    if (!cp || !trk || trk->kind != TrackKind::Video || (cp->kind != ClipKind::Media && cp->kind != ClipKind::Nested)) {
        emit message(tr("Select the video clip to stabilize."));
        return 0;
    }
    const Clip clip = *cp;
    stop();
    std::string err;
    auto gpu = GpuDevice::create(false, err);
    Compositor comp;
    if (!gpu || !comp.init(gpu, err)) {
        emit message(toQ(err));
        return 0;
    }
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, true);
    // the clip alone, untransformed
    RenderSnapshot snap = *snapshot(true);
    Clip plain = clip;
    plain.motion = makeEffect(fx::Motion);
    plain.opacity = makeEffect(fx::Opacity);
    plain.effects.clear();
    snap.seq.clips = {plain};
    snap.seq.transitions.clear();
    snap.showCaptions = false;
    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
    const int aw = 480, ah = std::max(16, int(std::lround(480.0 * H / std::max(1, W)))) & ~1;
    auto canvas = comp.makeCanvas(aw, ah, false);
    if (!canvas) return 0;
    std::vector<uint8_t> px, luma(size_t(aw) * ah);
    auto frameLuma = [&](Tick t) -> bool {
        renderer.render(snap, t, *canvas, provider, true);
        int pitch = 0;
        if (!comp.readback(*canvas, px, pitch)) return false;
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < aw; ++x) {
                const uint8_t* p = px.data() + size_t(y) * pitch + size_t(x) * 4;
                luma[size_t(y) * aw + x] = uint8_t((54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8);
            }
        return true;
    };
    const Tick fd = seq().frameDur();
    const Tick step = seq().settings.fps.toDouble() > 40 ? 2 * fd : fd;
    const Tick tEnd = clip.start + std::min(clip.duration - fd, 300 * kTicksPerSecond);
    // path of the picture's middle (analysis px); the tracker restarts where it loses the region
    std::vector<std::pair<Tick, QPointF>> path;
    RegionTracker tracker;
    QPointF offset(0, 0);
    const double rw = aw * 0.4, rh = ah * 0.4;
    for (Tick t = clip.start; t <= tEnd; t += step) {
        if (progress && !progress(double(t - clip.start) / double(std::max<Tick>(1, tEnd - clip.start)))) return 0;
        if (!frameLuma(t)) return 0;
        if (path.empty() || !tracker.track(luma.data(), aw, ah)) {
            if (!path.empty()) offset = path.back().second;  // re-anchor: continue from the last position
            tracker = RegionTracker();
            tracker.searchRadius = 40;
            if (!tracker.start(luma.data(), aw, ah, aw / 2.0, ah / 2.0, rw, rh)) {
                path.emplace_back(t - clip.start, path.empty() ? QPointF(0, 0) : path.back().second);
                continue;
            }
            path.emplace_back(t - clip.start, offset);
            continue;
        }
        path.emplace_back(t - clip.start, offset + QPointF(tracker.x() - aw / 2.0, tracker.y() - ah / 2.0));
    }
    if (path.size() < 3) {
        emit message(tr("The clip is too short to stabilize."));
        return 0;
    }
    // smooth path (moving average) - the correction removes what the smoothing takes out
    const int half = std::max(1, int(smoothSeconds * kTicksPerSecond / double(step) / 2));
    std::vector<QPointF> corr(path.size());
    double maxShift = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        QPointF sum(0, 0);
        int n = 0;
        for (int k = -half; k <= half; ++k) {
            const long j = std::clamp<long>(long(i) + k, 0, long(path.size()) - 1);
            sum += path[size_t(j)].second;
            ++n;
        }
        corr[i] = (sum / n - path[i].second) * (double(W) / aw);  // sequence px
        maxShift = std::max({maxShift, std::abs(corr[i].x()) / W, std::abs(corr[i].y()) / H});
    }
    const double zoom = std::clamp(1.0 + 2.2 * maxShift, 1.0, 1.25);  // hide the moving edges
    edit(tr("Stabilize"), [&](Project& p) {
        Clip* c = p.sequence.clip(clipId);
        if (!c) return;
        Param& sx = c->motion.params["scale"];
        Param& x = c->motion.params["posX"];
        Param& y = c->motion.params["posY"];
        const double s0 = sx.animated() ? sx.at(0) : sx.value;
        const double x0 = x.animated() ? x.at(0) : x.value, y0 = y.animated() ? y.at(0) : y.value;
        sx.keys.clear();
        sx.value = s0 * zoom;
        x.keys.clear();
        y.keys.clear();
        for (size_t i = 0; i < path.size(); ++i) {
            // the picture is zoomed: its shake on screen grows with it
            x.addKey(path[i].first, std::round((x0 + corr[i].x() * zoom * s0 / 100) * 10) / 10);
            y.addKey(path[i].first, std::round((y0 + corr[i].y() * zoom * s0 / 100) * 10) / 10);
        }
    });
    emit message(tr("Stabilized (zoomed %1 % to hide the edges) - Position keyframes are editable.").arg(int(std::lround((zoom - 1) * 100))), 8000);
    return int(path.size());
}

int Session::autoReframe(Id clipId, const std::function<bool(double)>& progress) {
    const Clip* cp = seq().clip(clipId);
    if (!cp || (cp->kind != ClipKind::Media && cp->kind != ClipKind::Nested)) {
        emit message(tr("Select the clip to reframe (e.g. the nested clip in a vertical version)."));
        return 0;
    }
    const Clip clip = *cp;
    // the clip's own picture size
    int sw = 0, sh = 0;
    if (clip.kind == ClipKind::Nested) {
        if (const Sequence* n = project().sequenceByUid(clip.nestedSeq)) sw = n->settings.width, sh = n->settings.height;
    } else if (const Asset* a = project().asset(clip.assetId)) {
        sw = a->info.video.width, sh = a->info.video.height;
    }
    const int W = seq().settings.width, H = seq().settings.height;
    const double fill = fitScale(FitMode::Fill, sw, sh, W, H);
    const double shown = sw * fill;  // displayed width at 100 %
    if (sw <= 0 || shown <= W + 1) {
        emit message(tr("The clip is not wider than the frame - nothing to reframe."));
        return 0;
    }
    stop();
    std::string err;
    auto gpu = GpuDevice::create(false, err);
    Compositor comp;
    if (!gpu || !comp.init(gpu, err)) {
        emit message(toQ(err));
        return 0;
    }
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, true);
    // the whole picture of the clip, untransformed, in a frame of its own shape
    RenderSnapshot snap = *snapshot(true);
    Clip plain = clip;
    plain.fit = FitMode::Fit;
    plain.motion = makeEffect(fx::Motion);
    plain.opacity = makeEffect(fx::Opacity);
    plain.effects.clear();
    snap.seq.settings.width = sw;
    snap.seq.settings.height = sh;
    snap.seq.clips = {plain};
    snap.seq.transitions.clear();
    snap.showCaptions = false;
    const int aw = 320, ah = std::max(16, int(std::lround(320.0 * sh / sw))) & ~1;
    auto canvas = comp.makeCanvas(aw, ah, false);
    if (!canvas) return 0;
    std::vector<uint8_t> px;
    auto lumaAt = [&](Tick t, std::vector<float>& out) -> bool {
        renderer.render(snap, t, *canvas, provider, true);
        int pitch = 0;
        if (!comp.readback(*canvas, px, pitch)) return false;
        out.resize(size_t(aw) * ah);
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < aw; ++x) {
                const uint8_t* p = px.data() + size_t(y) * pitch + size_t(x) * 4;
                out[size_t(y) * aw + x] = float(54 * p[0] + 183 * p[1] + 19 * p[2]) / 256.f;
            }
        return true;
    };
    // the crop window: the part of the picture the frame shows, in analysis px
    const int cw = std::max(4, int(std::lround(aw * double(W) / shown)));
    const Tick fd = seq().frameDur();
    const Tick step = kTicksPerSecond / 4;
    const Tick last = clip.start + clip.duration - fd;
    std::vector<std::pair<Tick, double>> centres;  // clip-local time, window centre (analysis px)
    std::vector<float> a, b;
    for (Tick t = clip.start; t <= last; t += step) {
        if (progress && !progress(double(t - clip.start) / double(std::max<Tick>(1, last - clip.start)))) return 0;
        // motion between two frames a little apart, plus some detail so still scenes centre on content
        if (!lumaAt(t, a) || !lumaAt(std::min(last, t + 3 * fd), b)) return 0;
        std::vector<double> energy(size_t(aw), 0.0);
        for (int y = 1; y < ah; ++y)
            for (int x = 1; x < aw; ++x) {
                const size_t i = size_t(y) * aw + x;
                energy[size_t(x)] += std::abs(b[i] - a[i]) + 0.05 * (std::abs(a[i] - a[i - 1]) + std::abs(a[i] - a[i - aw]));
            }
        centres.emplace_back(t - clip.start, bestWindow(energy, cw) + cw / 2.0);
    }
    if (centres.empty()) return 0;
    // smooth over about a second either way, then ease between keys every half second
    std::vector<double> smooth(centres.size());
    for (size_t i = 0; i < centres.size(); ++i) {
        double sum = 0;
        int n = 0;
        for (long k = long(i) - 4; k <= long(i) + 4; ++k)
            if (k >= 0 && k < long(centres.size())) sum += centres[size_t(k)].second, ++n;
        smooth[i] = sum / n;
    }
    const double limit = (shown - W) / 2.0;  // never show past the picture's edges
    int keys = 0;
    edit(tr("Auto Reframe"), [&](Project& p) {
        Clip* c = p.sequence.clip(clipId);
        if (!c) return;
        c->fit = FitMode::Fill;
        Param& x = c->motion.params["posX"];
        x.keys.clear();
        c->motion.params["scale"].keys.clear();
        c->motion.params["scale"].value = 100;
        for (size_t i = 0; i < centres.size(); i += 2) {
            const double srcX = smooth[i] / aw * sw;  // source px
            const double pos = std::clamp((sw / 2.0 - srcX) * fill, -limit, limit);
            x.addKey(centres[i].first, std::round(pos), Interp::Ease);
            ++keys;
        }
    });
    emit message(tr("Reframed: the frame follows the action (%n keyframe(s) on Position X).", "", keys), 6000);
    return keys;
}

int Session::pictureInPicture(int corner) {
    std::vector<Id> ids;
    for (Id id : selection_) {
        const Clip* c = seq().clip(id);
        const Track* t = c ? seq().track(c->trackId) : nullptr;
        if (c && t && t->kind == TrackKind::Video && (c->kind == ClipKind::Media || c->kind == ClipKind::Nested)) ids.push_back(id);
    }
    if (ids.empty()) {
        emit message(tr("Select the video clip to place (e.g. the facecam) first."));
        return 0;
    }
    edit(corner == 4 ? tr("Full Frame") : tr("Picture in Picture"), [&](Project& p) {
        Sequence& s = p.sequence;
        const int W = s.settings.width, H = s.settings.height;
        for (Id id : ids) {
            Clip* c = s.clip(id);
            int sw = W, sh = H;
            if (c->kind == ClipKind::Media) {
                if (const Asset* a = p.asset(c->assetId); a && a->info.video.width > 0) sw = a->info.video.width, sh = a->info.video.height;
            } else if (const Sequence* n = p.sequenceByUid(c->nestedSeq)) {
                sw = n->settings.width, sh = n->settings.height;
            }
            // size on screen at 100 %
            const Corners full = computeCorners(sw, sh, W, H, c->fit, MotionValues{});
            const double bw = std::abs(full.p[1][0] - full.p[0][0]), bh = std::abs(full.p[2][1] - full.p[0][1]);
            auto setStatic = [&](const char* id, double v) {
                Param& pr = c->motion.params[id];
                pr.keys.clear();
                pr.value = v;
            };
            setStatic("rotation", 0);
            setStatic("anchorX", 0);
            setStatic("anchorY", 0);
            c->motion.params["uniform"].value = 1;
            if (corner == 4 || bw <= 0 || bh <= 0) {
                setStatic("posX", 0);
                setStatic("posY", 0);
                setStatic("scale", 100);
                continue;
            }
            const double scale = 100.0 * 0.30 * W / bw;  // 30 % of the frame width
            const double pw = bw * scale / 100.0, ph = bh * scale / 100.0;
            const double margin = 0.035 * H;
            const double x = W / 2.0 - margin - pw / 2.0, y = H / 2.0 - margin - ph / 2.0;
            setStatic("scale", scale);
            setStatic("posX", corner == 0 || corner == 2 ? -x : x);
            setStatic("posY", corner < 2 ? -y : y);
            auto has = [&](const char* type) {
                for (const auto& e : c->effects)
                    if (e.type == type) return true;
                return false;
            };
            if (!has(fx::Rounded)) c->effects.push_back(makeEffect(fx::Rounded));
            if (!has(fx::DropShadow)) c->effects.push_back(makeEffect(fx::DropShadow));
        }
    });
    return int(ids.size());
}

void Session::animateSelection(int preset) {
    auto ids = selection_;
    int n = 0;
    edit(tr("Animation Preset"), [&](Project& p) {
        const auto& st = p.sequence.settings;
        for (Id id : ids)
            if (Clip* c = p.sequence.clip(id))
                n += edit::applyAnimation(*c, edit::Animation(preset), p.sequence.frameDur(), st.width, st.height) ? 1 : 0;
    });
    if (!n) emit message(tr("Select video clips, titles or mattes (at least 3 frames long) to animate."));
}

bool Session::canNest(Id uid) const {
    // dropping sequence `uid` into the open one must not make a sequence contain itself
    std::set<Id> seen;
    std::function<bool(Id)> contains = [&](Id u) -> bool {
        if (u == seq().uid) return true;
        if (!seen.insert(u).second) return false;
        const Sequence* s = project().sequenceByUid(u);
        if (!s) return false;
        for (const auto& c : s->clips)
            if (c.kind == ClipKind::Nested && contains(c.nestedSeq)) return true;
        return false;
    };
    return !contains(uid);
}

namespace {
// a sequence as a nested clip (video + linked audio when it has audio) at `at`
std::vector<Clip> nestedClipsFor(Sequence& s, const Sequence& src, Tick at, int vTrack, int aTrack) {
    const Tick dur = cut::snapToFrame(src.duration() + s.frameDur() - 1, s.settings.fps);
    bool hasAudio = false;
    for (const auto& c : src.clips)
        if (const Track* t = src.track(c.trackId); t && t->kind == TrackKind::Audio) hasAudio = true;
    std::vector<Clip> clips;
    auto make = [&](TrackKind k, int idx) {
        Clip n;
        n.kind = ClipKind::Nested;
        n.nestedSeq = src.uid;
        n.name = src.name;
        n.trackId = edit::ensureTracks(s, k, idx + 1)[size_t(idx)];
        n.start = at;
        n.duration = dur;
        initBuiltinEffects(n, k);
        clips.push_back(n);
    };
    make(TrackKind::Video, std::max(0, vTrack));
    if (hasAudio) make(TrackKind::Audio, std::max(0, aTrack));
    if (clips.size() == 2) clips[0].linkId = clips[1].linkId = s.newId();
    return clips;
}
} // namespace

std::vector<Id> Session::placeSequence(Id uid, Tick at, int vTrack, int aTrack, bool insert) {
    const Sequence* src = project().sequenceByUid(uid);
    if (!src || src->duration() <= 0) {
        emit message(tr("That sequence is empty."));
        return {};
    }
    if (!canNest(uid)) {
        emit message(tr("A sequence cannot contain itself."));
        return {};
    }
    const Sequence srcCopy = *src;
    std::vector<Id> ids;
    edit(tr("Nest Sequence"), [&](Project& p) {
        Sequence& s = p.sequence;
        auto clips = nestedClipsFor(s, srcCopy, at, vTrack, aTrack);
        ids = insert ? edit::insertClips(s, clips, at) : edit::overwriteClips(s, clips);
    });
    setSelection(ids, false);
    return ids;
}

void Session::createVerticalVersion() {
    if (seq().duration() <= 0) {
        emit message(tr("The sequence is empty."));
        return;
    }
    stop();
    const Sequence src = seq();
    SequenceSettings vs = src.settings;
    vs.width = 1080;
    vs.height = 1920;
    const QString n = uniqueName(sequenceNames(), toQ(src.name) + tr(" (Vertical)"));
    const int at = project().activeSequence + 1;
    const Tick head = playhead_;
    std::vector<Id> ids;
    edit(tr("Create Vertical Version"), [&](Project& p) {
        Sequence v = makeDefaultSequence(vs);
        v.name = toS(n);
        v.markers = src.markers;
        edit::insertSequence(p, at, std::move(v));
        Sequence& s = p.sequence;
        auto clips = nestedClipsFor(s, src, 0, 0, 0);
        clips.front().fit = FitMode::Fill;  // centre crop; Motion > Position reframes
        ids = edit::overwriteClips(s, clips);
    });
    setSelection(ids, false);
    setPlayhead(head);
    emit projectLoaded();
    emit message(tr("Vertical version created - Clip > Auto Reframe makes it follow the action (or drag Position yourself)."), 8000);
}

void Session::nestSelection() {
    if (selection_.empty()) {
        emit message(tr("Select the clips to nest first."));
        return;
    }
    std::vector<Id> ids;
    for (Id id : selection_)
        for (Id l : seq().linkedClips(id)) ids.push_back(l);
    const QString name = uniqueName(sequenceNames(), tr("Nested Sequence"));
    std::vector<Id> nests;
    edit(tr("Nest"), [&](Project& p) { nests = edit::nestClips(p, ids, toS(name)); });
    if (!nests.empty()) {
        setSelection(nests, false);
        emit message(tr("Nested into \"%1\" - double-click the clip to edit inside it.").arg(name));
    }
}

int Session::tabOfSequence(Id uid) const {
    if (seq().uid == uid) return project().activeSequence;
    const auto& others = project().otherSequences;
    for (int i = 0; i < int(others.size()); ++i)
        if (others[size_t(i)].uid == uid) return i < project().activeSequence ? i : i + 1;
    return -1;
}

void Session::openNested(Id clipId) {
    const Clip* c = seq().clip(clipId);
    if (!c || c->kind != ClipKind::Nested) return;
    const int tab = tabOfSequence(c->nestedSeq);
    if (tab < 0) emit message(tr("The nested sequence was deleted."));
    else switchSequence(tab);
}
void Session::newSequence(const SequenceSettings& st, const QString& name) {
    stop();
    const QString n = uniqueName(sequenceNames(), name.isEmpty() ? tr("Sequence") : name);
    const int at = project().activeSequence + 1;
    edit(tr("New Sequence"), [&](Project& p) {
        Sequence s = makeDefaultSequence(st);
        s.name = toS(n);
        edit::insertSequence(p, at, std::move(s));
    });
    setSelection({}, false);
    setPlayhead(0);
    emit projectLoaded();
}

void Session::duplicateSequence() {
    stop();
    const QString n = uniqueName(sequenceNames(), toQ(seq().name) + tr(" Copy"));
    const int at = project().activeSequence + 1;
    edit(tr("Duplicate Sequence"), [&](Project& p) {
        Sequence s = p.sequence;
        s.name = toS(n);
        edit::insertSequence(p, at, std::move(s));
    });
    emit projectLoaded();
}

void Session::renameSequence(int tab, const QString& name) {
    if (name.trimmed().isEmpty()) return;
    edit(tr("Rename Sequence"), [&](Project& p) { edit::renameSequence(p, tab, toS(name.trimmed())); });
}

bool Session::deleteSequence(int tab) {
    if (project().otherSequences.empty()) {
        emit message(tr("A project keeps at least one sequence."));
        return false;
    }
    stop();
    edit(tr("Delete Sequence"), [&](Project& p) { edit::removeSequence(p, tab); });
    seqPlayheads_.clear();
    setSelection({}, false);
    emit projectLoaded();
    return true;
}
// ---------------------------------------------------------------------------------------------
// Effect presets (QSettings "effectPresets/<name>" = effect JSON)

QStringList Session::effectPresets() const {
    QSettings st;
    st.beginGroup("effectPresets");
    QStringList names = st.childKeys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

namespace {
struct Look {
    const char* name;
    double exposure, contrast, highlights, shadows, saturation, temperature, tint;
};
const Look kLooks[] = {
    {"Punchy", 0, 25, -10, 0, 125, 0, 0},
    {"Vivid Gaming", 0, 15, -5, -10, 145, 0, 0},
    {"Warm Sunset", 0.1, 5, 0, 5, 110, 30, 5},
    {"Cool Blue", 0, 5, 0, 0, 95, -30, 0},
    {"Faded Film", 0, -25, -15, 20, 75, 8, 0},
    {"Night", -0.6, 10, -20, -5, 70, -45, 5},
    {"Bleach Bypass", 0, 30, 0, -10, 40, 0, 0},
};
} // namespace

QStringList Session::lookNames() {
    QStringList n;
    for (const Look& l : kLooks) n << l.name;
    return n;
}

bool Session::effectPreset(const QString& name, Effect& out) const {
    if (name.startsWith("look:")) {
        for (const Look& l : kLooks)
            if (name.mid(5) == l.name) {
                out = makeEffect(fx::Color);
                const std::pair<const char*, double> v[] = {{"exposure", l.exposure},     {"contrast", l.contrast},
                                                            {"highlights", l.highlights}, {"shadows", l.shadows},
                                                            {"saturation", l.saturation}, {"temperature", l.temperature},
                                                            {"tint", l.tint}};
                for (const auto& [id, val] : v) out.params[id].value = val;
                return true;
            }
        return false;
    }
    return effectFromJson(toS(QSettings().value("effectPresets/" + name).toString()), out);
}

void Session::saveEffectPreset(const QString& name, const Effect& e) {
    QSettings().setValue("effectPresets/" + name, toQ(effectToJson(e)));
    emit effectPresetsChanged();
}

void Session::removeEffectPreset(const QString& name) {
    QSettings().remove("effectPresets/" + name);
    emit effectPresetsChanged();
}

int Session::applyEffect(const std::vector<Id>& ids, const Effect& e, const QString& label) {
    int n = 0;
    edit(label, [&](Project& p) {
        for (Id id : ids) {
            Clip* c = p.sequence.clip(id);
            const Track* t = c ? p.sequence.track(c->trackId) : nullptr;
            if (!t) continue;
            const EffectDef* def = findEffectDef(e.type);
            const bool audio = e.type == fx::Volume || (def && !def->video);
            if (audio != (t->kind == TrackKind::Audio) || t->kind == TrackKind::Caption) continue;
            if (e.type == fx::Motion) c->motion = e;
            else if (e.type == fx::Opacity) c->opacity = e;
            else if (e.type == fx::Volume) c->volume = e;
            else {
                auto it = std::find_if(c->effects.begin(), c->effects.end(), [&](const Effect& x) { return x.type == e.type; });
                if (it != c->effects.end()) *it = e;  // one instance per type: the preset replaces it
                else c->effects.push_back(e);
            }
            ++n;
        }
    });
    return n;
}
void Session::closeAllGaps() {
    int closed = 0;
    edit(tr("Close Gaps"), [&](Project& p) {
        Sequence& s = p.sequence;
        // spans where anything exists on any track (captions included); only empty time goes
        std::vector<std::pair<Tick, Tick>> iv;
        for (const auto& c : s.clips) iv.push_back({c.start, c.end()});
        for (const auto& c : s.captions) iv.push_back({c.start, c.end});
        std::sort(iv.begin(), iv.end());
        std::vector<std::pair<Tick, Tick>> gaps;
        Tick cursor = 0;
        for (const auto& [a, b] : iv) {
            if (a > cursor) gaps.push_back({cursor, a});
            cursor = std::max(cursor, b);
        }
        for (auto it = gaps.rbegin(); it != gaps.rend(); ++it)
            if (edit::rippleShiftLeft(s, it->second, it->second - it->first) > 0) ++closed;
    });
    emit message(closed ? tr("Closed %n gap(s)", "", closed) : tr("No gaps to close"));
}

void Session::pasteAttributes(bool motion, bool opacity, bool effects, bool volume) {
    const Clip* vsrc = nullptr;
    const Clip* asrc = nullptr;
    for (const Clip& c : clipboard_) {
        if (!vsrc && !c.motion.type.empty()) vsrc = &c;
        if (!asrc && !c.volume.type.empty()) asrc = &c;
    }
    if ((!vsrc && !asrc) || selection_.empty()) {
        emit message(tr("Copy a clip first (Ctrl+C), then select the clips to paste its attributes onto."));
        return;
    }
    auto ids = selection_;
    edit(tr("Paste Attributes"), [&](Project& p) {
        for (Id id : ids) {
            Clip* c = p.sequence.clip(id);
            if (!c) continue;
            if (vsrc && !c->motion.type.empty() && c->id != vsrc->id) {
                if (motion) c->motion = vsrc->motion;
                if (opacity) c->opacity = vsrc->opacity;
                if (effects) c->effects = vsrc->effects;
            }
            if (asrc && !c->volume.type.empty() && c->id != asrc->id && volume) {
                c->volume = asrc->volume;
                c->gainDb = asrc->gainDb;
                c->fadeIn = std::min(asrc->fadeIn, c->duration);
                c->fadeOut = std::min(asrc->fadeOut, c->duration - c->fadeIn);
            }
        }
    });
}
// Clears [a,b) on every unlocked track (captions included) and optionally closes the gap.
static void removeRange(Sequence& s, Tick a, Tick b, bool ripple) {
    for (const auto& t : s.tracks) {
        if (t.locked) continue;
        if (t.kind == TrackKind::Caption) {
            std::vector<Id> del;
            for (auto& c : s.captions) {
                if (c.start >= a && c.end <= b) del.push_back(c.id);
                else if (c.start < a && c.end > a) c.end = a;
                else if (c.start < b && c.end > b) c.start = b;
            }
            edit::deleteCaptions(s, del);
            continue;
        }
        edit::clearRange(s, t.id, a, b);
    }
    if (ripple) edit::rippleShiftLeft(s, a, b - a);
}

void Session::setSelectedLabel(uint32_t argb) {
    if (selection_.empty()) return;
    auto ids = selection_;
    edit(tr("Label"), [&](Project& p) {
        for (Id id : ids)
            if (Clip* c = p.sequence.clip(id)) c->label = argb;
    });
}

void Session::liftExtract(bool extract) {
    const Sequence& sq = seq();
    if (sq.inPoint == kInvalidTick || sq.outPoint == kInvalidTick || sq.outPoint <= sq.inPoint) {
        emit message(tr("Set sequence In and Out first (I / O)."));
        return;
    }
    const Tick a = sq.inPoint, b = sq.outPoint;
    edit(extract ? tr("Extract") : tr("Lift"), [&](Project& p) {
        removeRange(p.sequence, a, b, extract);
        if (extract) p.sequence.outPoint = kInvalidTick;
    });
    setPlayhead(a);
}

void Session::rippleTrimToPlayhead(bool previous) {
    const Sequence& sq = seq();
    const Tick t = snapToFrame(playhead_);
    // the nearest edit of the clips under the playhead; everything in between goes on all tracks
    Tick a = kInvalidTick, b = kInvalidTick;
    for (const auto& c : sq.clips) {
        const Track* tr = sq.track(c.trackId);
        if (!tr || tr->locked || !(c.start < t && c.end() > t)) continue;
        if (previous) a = a == kInvalidTick ? c.start : std::max(a, c.start);
        else b = b == kInvalidTick ? c.end() : std::min(b, c.end());
    }
    if (previous ? a == kInvalidTick : b == kInvalidTick) {
        emit message(tr("Place the playhead over a clip to ripple trim it."));
        return;
    }
    if (previous) b = t;
    else a = t;
    edit(previous ? tr("Ripple Trim Previous Edit") : tr("Ripple Trim Next Edit"),
         [&](Project& p) { removeRange(p.sequence, a, b, true); });
    setPlayhead(a);
}

void Session::matchFrame() {
    Tick t = playhead_;
    const Sequence* sq = &seq();
    const Clip* hit = nullptr;
    if (Id id = primaryClip())
        if (const Clip* c = sq->clip(id); c && c->contains(t) && c->kind != ClipKind::Text) hit = c;
    // topmost video clip under t (disabled clips match too); nests are looked through
    for (int depth = 0; depth < 4; ++depth) {
        if (!hit) {
            auto vts = sq->tracksOf(TrackKind::Video);
            for (auto it = vts.rbegin(); it != vts.rend() && !hit; ++it)
                for (const Clip* c : sq->clipsOnTrack((*it)->id))
                    if (c->contains(t) && c->kind != ClipKind::Text) hit = c;
        }
        if (!hit || hit->kind != ClipKind::Nested) break;
        const Sequence* inner = project().sequenceByUid(hit->nestedSeq);
        if (!inner) {
            hit = nullptr;
            break;
        }
        t = hit->sourceAt(t, sq->frameDur());
        sq = inner;
        hit = nullptr;
    }
    if (!hit || hit->kind != ClipKind::Media) {
        emit message(tr("No media clip under the playhead."));
        return;
    }
    const Tick src = hit->sourceAt(t, sq->frameDur());
    openInSource(hit->assetId);
    setSourcePosition(src);
}

int Session::autoColorSelection() {
    std::vector<Id> ids;
    for (Id id : selection_) {
        const Clip* c = seq().clip(id);
        const Track* t = c ? seq().track(c->trackId) : nullptr;
        if (c && t && t->kind == TrackKind::Video && (c->kind == ClipKind::Media || c->kind == ClipKind::Nested)) ids.push_back(id);
    }
    if (ids.empty()) {
        emit message(tr("Select the video clips to correct first."));
        return 0;
    }
    std::string err;
    auto gpu = GpuDevice::create(false, err);
    Compositor comp;
    if (!gpu || !comp.init(gpu, err)) {
        emit message(toQ(err));
        return 0;
    }
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, true);
    const SnapshotPtr base = snapshot(true);
    const int W = base->seq.settings.width, H = base->seq.settings.height;
    const int cw = 480, ch = std::max(16, int(std::lround(480.0 * H / std::max(1, W)))) & ~1;
    auto canvas = comp.makeCanvas(cw, ch, false);
    if (!canvas) return 0;
    std::map<Id, ColorFix> fixes;
    for (Id id : ids) {
        // the clip alone, full frame, without its own colour correction, at the playhead (or its middle)
        RenderSnapshot snap = *base;
        const Clip* src = snap.seq.clip(id);
        if (!src) continue;
        Clip c = *src;
        std::erase_if(c.effects, [](const Effect& e) { return e.type == fx::Color; });
        c.motion = makeEffect(fx::Motion);
        c.opacity = makeEffect(fx::Opacity);
        const Tick t = playhead_ >= c.start && playhead_ < c.end() ? playhead_ : c.start + c.duration / 2;
        snap.seq.clips = {c};
        snap.seq.transitions.clear();
        snap.showCaptions = false;
        renderer.render(snap, t, *canvas, provider, true);
        std::vector<uint8_t> px;
        int pitch = 0;
        if (!comp.readback(*canvas, px, pitch)) continue;
        const ColorFix f = autoColor(px.data(), cw, ch, pitch);
        if (f.ok) fixes[id] = f;
    }
    if (fixes.empty()) {
        emit message(tr("Nothing to analyse there."));
        return 0;
    }
    edit(tr("Auto Color"), [&](Project& p) {
        for (const auto& [id, f] : fixes) {
            Clip* c = p.sequence.clip(id);
            if (!c) continue;
            Effect* cc = nullptr;
            for (auto& e : c->effects)
                if (e.type == fx::Color) cc = &e;
            if (!cc) {
                c->effects.insert(c->effects.begin(), makeEffect(fx::Color));
                cc = &c->effects.front();
            }
            cc->enabled = true;
            auto setStatic = [&](const char* name, double v) {
                Param& pr = cc->params[name];
                pr.keys.clear();
                pr.value = v;
            };
            setStatic("exposure", f.exposure);
            setStatic("contrast", f.contrast);
            setStatic("temperature", f.temperature);
            setStatic("tint", f.tint);
        }
    });
    const ColorFix& f = fixes.begin()->second;
    emit message(tr("Auto Color: temperature %1, tint %2, exposure %3, contrast %4 - fine-tune in Effect Controls.")
                     .arg(f.temperature).arg(f.tint).arg(f.exposure, 0, 'f', 2).arg(f.contrast), 8000);
    return int(fixes.size());
}

bool Session::exportFrame(const QString& path, QString& error) {
    std::string err;
    auto gpu = GpuDevice::create(false, err);
    if (!gpu) {
        error = toQ(err);
        return false;
    }
    Compositor comp;
    if (!comp.init(gpu, err)) {
        error = toQ(err);
        return false;
    }
    FrameRenderer renderer(comp);
    SyncFrameProvider provider(gpu, true);
    SnapshotPtr snap = snapshot(true);
    const int W = snap->seq.settings.width, H = snap->seq.settings.height;
    auto canvas = comp.makeCanvas(W, H, false);
    renderer.render(*snap, playhead_, *canvas, provider, true);
    std::vector<uint8_t> px;
    int pitch = 0;
    if (!comp.readback(*canvas, px, pitch)) {
        error = tr("GPU readback failed");
        return false;
    }
    QImage img(W, H, QImage::Format_RGB32);
    for (int y = 0; y < H; ++y) {
        const uint8_t* s = px.data() + size_t(y) * pitch;
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < W; ++x) d[x] = qRgb(s[x * 4], s[x * 4 + 1], s[x * 4 + 2]);  // premultiplied over black
    }
    if (!img.save(path)) {
        error = tr("Could not write %1").arg(path);
        return false;
    }
    emit message(tr("Saved frame %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

bool Session::exportThumbnail(const QString& path, QString& error) {
    const QString tmp = QDir::temp().filePath(QString("cutline_thumb_%1.png").arg(QCoreApplication::applicationPid()));
    if (!exportFrame(tmp, error)) return false;
    QImage full(tmp);
    QFile::remove(tmp);
    if (full.isNull()) {
        error = tr("Could not render the frame.");
        return false;
    }
    const QImage img = full.scaled(1280, 720, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QImage thumb = img.copy((img.width() - 1280) / 2, (img.height() - 720) / 2, 1280, 720);
    for (int q : {95, 90, 85, 75, 60}) {  // YouTube takes up to 2 MB
        if (!thumb.save(path, "JPG", q)) {
            error = tr("Could not write %1").arg(path);
            return false;
        }
        if (QFileInfo(path).size() < 2'000'000) break;
    }
    emit message(tr("Saved thumbnail %1 (1280x720)").arg(QFileInfo(path).fileName()), 4000);
    return true;
}

void Session::normalizeSelectedAudio(double targetLufs) {
    std::vector<Clip> clips;
    for (Id id : selection_)
        if (const Clip* c = seq().clip(id)) {
            const Track* t = seq().track(c->trackId);
            if (t && t->kind == TrackKind::Audio && c->kind == ClipKind::Media) clips.push_back(*c);
        }
    if (clips.empty()) {
        emit message(tr("Select audio clips to normalize."));
        return;
    }
    emit message(tr("Measuring loudness of %1 clip(s)...").arg(clips.size()), 3000);
    SnapshotPtr snap = snapshot(true);
    jobs_->submit("loudness", [this, clips, snap, targetLufs](JobToken& tok) {
        std::vector<std::pair<Id, double>> gains;
        for (const Clip& c : clips) {
            const AssetRef* a = snap->asset(c.assetId);
            if (!a || c.stream < 0 || size_t(c.stream) >= a->info.audio.size()) continue;
            ClipAudio audio;
            std::string err;
            if (!audio.open(a->path, a->info.audio[size_t(c.stream)].index, a->info.origin, err)) continue;
            Clip raw = c;  // measure the clip's media at unity gain (speed/reverse as edited)
            LoudnessMeter meter;
            std::vector<float> buf(4800 * 2);
            const int64_t s0 = ticksToSamples(c.start, 48000), s1 = ticksToSamples(c.end(), 48000);
            for (int64_t s = s0; s < s1; s += 4800) {
                if (!tok.checkpoint()) return;
                int n = int(std::min<int64_t>(4800, s1 - s));
                audio.render(raw, s, buf.data(), n);
                meter.push(buf.data(), n);
            }
            double l = meter.integrated();
            if (std::isfinite(l)) gains.push_back({c.id, std::clamp(targetLufs - l, -40.0, 24.0)});
        }
        QMetaObject::invokeMethod(this, [this, gains, targetLufs] {
            if (gains.empty()) {
                emit message(tr("Nothing to normalize (silent clips?)."));
                return;
            }
            edit(tr("Normalize Audio"), [&](Project& p) {
                for (auto [id, g] : gains)
                    if (Clip* c = p.sequence.clip(id)) c->gainDb = std::round(g * 10) / 10;
            });
            emit message(tr("Normalized %1 clip(s) to %2 LUFS").arg(gains.size()).arg(targetLufs), 4000);
        });
    });
}

void Session::setSelectedGain(double db, bool relative) {
    auto ids = selection_;
    edit(tr("Audio Gain"), [&](Project& p) {
        for (Id id : ids)
            if (Clip* c = p.sequence.clip(id)) {
                const Track* t = p.sequence.track(c->trackId);
                if (!t || t->kind != TrackKind::Audio) continue;
                c->gainDb = std::clamp(relative ? c->gainDb + db : db, -60.0, 24.0);
            }
    });
}

void Session::renameClip(Id id, const QString& name) {
    edit(tr("Rename Clip"), [&](Project& p) {
        if (Clip* c = p.sequence.clip(id)) c->name = toS(name);
    });
}

void Session::checkSequenceMatch(Id asset, bool sequenceWasEmpty) {
    if (!sequenceWasEmpty) return;
    const Asset* a = doc_.project.asset(asset);
    if (!a || !a->info.hasVideo || a->info.isImage) return;
    const auto& st = seq().settings;
    const bool sizeDiff = a->info.video.width != st.width || a->info.video.height != st.height;
    const bool fpsDiff = a->info.video.fps.valid() && std::abs(a->info.video.fps.toDouble() - st.fps.toDouble()) > 0.01;
    if (sizeDiff || fpsDiff) emit sequenceMismatch(asset);
}

void Session::importAndPlace(const QStringList& files, Tick at, int vTrack, int aTrack, bool insert) {
    importFiles(files, 0, [this, at, vTrack, aTrack, insert](const std::vector<Id>& ids) {
        if (ids.empty()) return;
        const bool wasEmpty = seq().clips.empty();
        Tick t = at;
        edit(tr("Add Clips"), [&](Project& p) {
            for (Id id : ids) {
                const Asset* a = p.asset(id);
                if (!a) continue;
                Asset copy = *a;
                Tick dur = copy.info.isImage ? kDefaultStillDuration : copy.info.duration;
                auto clips = edit::clipsForAsset(p.sequence, copy, t, 0, dur, vTrack, aTrack);
                Tick end = t;
                for (const auto& c : clips) end = std::max(end, c.end());
                if (insert) edit::insertClips(p.sequence, clips, t);
                else edit::overwriteClips(p.sequence, clips);
                t = end;
            }
        });
        checkSequenceMatch(ids.front(), wasEmpty);
    });
}
// ---------------------------------------------------------------------------------------------
// Synchronize by audio: the earliest selected clip stays, the others move so their sound lines up.

namespace {
constexpr double kEnvRate = 200.0;  // envelope values per second (5 ms)

bool syncableAudio(const Clip& c, const Sequence& s) {
    const Track* t = s.track(c.trackId);
    return t && t->kind == TrackKind::Audio && c.kind == ClipKind::Media && c.speed == 1.0 && !c.reverse;
}

// mono 8 kHz of [from, from + len) seconds of the clip's source
std::vector<float> decode8k(const Asset& a, int stream, double from, double len) {
    std::vector<float> out;
    AudioReader r;
    std::string err;
    if (stream < 0 || stream >= int(a.info.audio.size()) || !r.open(a.path, a.info.audio[size_t(stream)].index, a.info.origin, err))
        return out;
    const int frames = int(len * 48000) / 6 * 6;
    std::vector<float> st(size_t(frames) * 2);
    r.read(int64_t(std::llround(from * 48000)), st.data(), frames);
    out.resize(size_t(frames / 6));
    for (size_t i = 0; i < out.size(); ++i) {
        float sum = 0;
        for (size_t k = 0; k < 6; ++k) sum += st[(i * 6 + k) * 2] + st[(i * 6 + k) * 2 + 1];
        out[i] = sum / 12.f;
    }
    return out;
}
} // namespace

int Session::synchronizeSelection() {
    // link groups among the selection, each with its audio clip
    struct Group {
        std::vector<Id> ids;
        Id audio = 0;
        Tick start = 0;
        int videoTrack = 1 << 20;
    };
    std::vector<Group> groups;
    std::set<Id> seen;
    for (Id id : selection_) {
        if (seen.count(id)) continue;
        Group g;
        g.start = std::numeric_limits<Tick>::max();
        for (Id l : seq().linkedClips(id)) {
            seen.insert(l);
            const Clip* c = seq().clip(l);
            if (!c) continue;
            g.ids.push_back(l);
            g.start = std::min(g.start, c->start);
            if (!g.audio && syncableAudio(*c, seq())) g.audio = l;
            if (const Track* t = seq().track(c->trackId); t && t->kind == TrackKind::Video)
                g.videoTrack = std::min(g.videoTrack, seq().trackIndexInKind(t->id));
        }
        if (!g.ids.empty()) groups.push_back(g);
    }
    if (groups.size() < 2) {
        emit message(tr("Select two or more clips with audio (e.g. the gameplay and the separately recorded mic)."), 6000);
        return 0;
    }
    for (const auto& g : groups)
        if (!g.audio) {
            emit message(tr("Every selected clip needs audio at normal speed to synchronize by sound."), 6000);
            return 0;
        }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.start != b.start ? a.start < b.start : a.videoTrack < b.videoTrack;
    });

    auto envelope = [&](const Clip& c, std::vector<float>& env) -> bool {
        WaveformPtr wf = waveform(c.assetId, c.stream);
        if (!wf || !wf->complete.load()) return false;
        const int64_t s0 = ticksToSamples(c.sourceIn, 48000);
        const int64_t n = ticksToSamples(c.duration, 48000) / 240;
        env.assign(size_t(std::max<int64_t>(0, n)), 0.f);
        for (int64_t i = 0; i < n; ++i) {
            float mn = 0, mx = 0;
            if (wf->range(s0 + i * 240, s0 + (i + 1) * 240, mn, mx)) env[size_t(i)] = std::max(std::abs(mn), std::abs(mx));
        }
        return true;
    };
    const Clip ref = *seq().clip(groups[0].audio);
    const Asset* refAsset = project().asset(ref.assetId);
    std::vector<float> xEnv;
    if (!refAsset || !envelope(ref, xEnv)) {
        emit message(tr("The audio is still being analysed - try again in a moment."));
        return 0;
    }
    struct Move {
        std::vector<Id> ids;
        Tick exact = 0;  // where the other audio clip's first sample belongs
        Id audio = 0;
    };
    std::vector<Move> moves;
    int unsure = 0;
    for (size_t gi = 1; gi < groups.size(); ++gi) {
        const Clip oc = *seq().clip(groups[gi].audio);
        const Asset* oa = project().asset(oc.assetId);
        std::vector<float> yEnv;
        if (!oa || !envelope(oc, yEnv)) {
            emit message(tr("The audio is still being analysed - try again in a moment."));
            return 0;
        }
        const SyncMatch m = matchEnvelopes(xEnv, yEnv, kEnvRate);
        if (!m.ok || m.confidence < 1.15) {
            ++unsure;
            continue;
        }
        // sharpen on the loudest 6 s the two share
        double lag = m.lagSec;
        const double yDur = double(yEnv.size()) / kEnvRate, xDur = double(xEnv.size()) / kEnvRate;
        const double v0min = std::max(0.0, -lag), v1max = std::min(yDur, xDur - lag);
        const double win = std::min(6.0, v1max - v0min - 0.1);
        if (win > 0.5) {
            const size_t w = size_t(win * kEnvRate);
            const size_t a0 = size_t(v0min * kEnvRate), a1 = std::min(yEnv.size(), size_t(v1max * kEnvRate));
            size_t bestI = a0;
            double bestSum = -1, sum = 0;
            for (size_t i = a0; i < a1; ++i) {
                sum += yEnv[i];
                if (i >= a0 + w) sum -= yEnv[i - w];
                if (i + 1 >= a0 + w && sum > bestSum) bestSum = sum, bestI = i + 1 - w;
            }
            const double v0 = double(bestI) / kEnvRate;
            constexpr double kMargin = 0.03;
            const double xs = v0 + lag - kMargin;
            if (xs >= 0) {
                const auto y8 = decode8k(*oa, oc.stream, ticksToSeconds(oc.sourceIn) + v0, win);
                const auto x8 = decode8k(*refAsset, ref.stream, ticksToSeconds(ref.sourceIn) + xs, win + 2 * kMargin);
                if (!y8.empty() && !x8.empty()) {
                    const int center = int(kMargin * 8000);
                    lag = lag - kMargin + double(refineLag(x8, y8, center - 80, center + 80)) / 8000.0;
                }
            }
        }
        // the other audio clip's first sample belongs at ref.start + lag
        Move mv;
        mv.ids = groups[gi].ids;
        mv.audio = oc.id;
        mv.exact = ref.start + secondsToTicks(lag);
        if (groups[gi].start + (mv.exact - oc.start) < 0) {
            ++unsure;
            continue;
        }
        moves.push_back(mv);
    }
    if (moves.empty()) {
        emit message(tr("No common sound found - are these recordings of the same moment?"), 6000);
        return 0;
    }
    edit(tr("Synchronize"), [&](Project& p) {
        Sequence& s = p.sequence;
        const Tick F = s.frameDur();
        for (const auto& mv : moves) {
            // Clips sit on frames; the part of the offset smaller than a frame goes into the source
            // in-points. Snapping to the earlier frame moves them back (needs source before the
            // in-point), to the later one forward (needs source after the out-point, else the
            // tail loses a frame).
            const Tick before = cut::floorToFrame(mv.exact, s.settings.fps);
            bool roomBefore = true;
            for (Id id : mv.ids)
                if (const Clip* c = s.clip(id); c && c->kind == ClipKind::Media && c->sourceIn < mv.exact - before) roomBefore = false;
            const Tick snapped = roomBefore || before == mv.exact ? before : before + F;
            const Tick residual = mv.exact - snapped;  // > 0: in-points back, < 0: forward
            const Clip* a0 = s.clip(mv.audio);
            if (!a0) continue;
            const Tick dt = snapped - a0->start;
            if (dt != 0) edit::moveClips(s, mv.ids, dt, 0, 0);
            for (Id id : mv.ids) {
                Clip* c = s.clip(id);
                const Asset* a = c ? p.asset(c->assetId) : nullptr;
                if (!c || !a || c->kind != ClipKind::Media || c->speed != 1.0 || c->reverse || residual == 0) continue;
                const Tick in = c->sourceIn - residual;
                if (in < 0) continue;
                if (in + c->sourceSpan() > a->info.duration && c->duration > F) c->duration -= F;
                if (in + c->sourceSpan() <= a->info.duration) c->sourceIn = in;
            }
        }
    });
    if (unsure)
        emit message(tr("Synchronized %1 clip(s); %2 had no clear match and stayed put.").arg(moves.size()).arg(unsure), 8000);
    else
        emit message(tr("Synchronized %1 clip(s) by audio.").arg(moves.size()), 5000);
    return int(moves.size());
}

} // namespace cut::ui
