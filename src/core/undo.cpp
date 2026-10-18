#include "core/undo.h"

#include "core/edits.h"

#include <algorithm>
#include <map>

namespace cut {

namespace {
Project header(const Project& p) {
    Project h;
    h.name = p.name;
    h.settings = p.settings;
    h.bins = p.bins;
    h.assets = p.assets;
    h.nextAssetId = p.nextAssetId;
    h.otherSequences = p.otherSequences;
    h.activeSequence = p.activeSequence;
    h.nextSequenceUid = p.nextSequenceUid;
    const Sequence& s = p.sequence;
    Sequence& hs = h.sequence;
    hs.name = s.name;
    hs.uid = s.uid;
    hs.settings = s.settings;
    hs.tracks = s.tracks;
    hs.transitions = s.transitions;
    hs.markers = s.markers;
    hs.captions = s.captions;
    hs.captionStyle = s.captionStyle;
    hs.inPoint = s.inPoint;
    hs.outPoint = s.outPoint;
    hs.masterVolumeDb = s.masterVolumeDb;
    hs.nextId = s.nextId;
    return h;
}

bool headerEqual(const Project& a, const Project& b) {
    const Sequence& x = a.sequence;
    const Sequence& y = b.sequence;
    return a.name == b.name && a.settings == b.settings && a.bins == b.bins && a.assets == b.assets &&
           a.nextAssetId == b.nextAssetId && a.otherSequences == b.otherSequences && a.activeSequence == b.activeSequence && a.nextSequenceUid == b.nextSequenceUid && x.uid == y.uid && x.name == y.name && x.settings == y.settings && x.tracks == y.tracks &&
           x.transitions == y.transitions && x.markers == y.markers && x.captions == y.captions &&
           x.captionStyle == y.captionStyle && x.inPoint == y.inPoint && x.outPoint == y.outPoint &&
           x.masterVolumeDb == y.masterVolumeDb && x.nextId == y.nextId;
}
} // namespace

size_t ProjectDiff::approxBytes() const {
    size_t b = sizeof(*this) + clips.size() * (2 * sizeof(Clip));
    if (headerBefore)
        b += 2 * (headerBefore->assets.size() * sizeof(Asset) + headerBefore->sequence.captions.size() * 64 +
                  headerBefore->sequence.tracks.size() * sizeof(Track));
    if (headerBefore)
        for (const auto& s : headerBefore->otherSequences) b += 2 * s.clips.size() * sizeof(Clip);
    return b;
}

ProjectDiff makeDiff(const Project& before, const Project& after) {
    ProjectDiff d;
    if (!headerEqual(before, after)) {
        d.headerBefore = header(before);
        d.headerAfter = header(after);
    }
    std::map<Id, const Clip*> a, b;
    for (const auto& c : before.sequence.clips) a[c.id] = &c;
    for (const auto& c : after.sequence.clips) b[c.id] = &c;
    for (const auto& [id, c] : a) {
        auto it = b.find(id);
        if (it == b.end()) d.clips.push_back({*c, std::nullopt});
        else if (!(*it->second == *c)) d.clips.push_back({*c, *it->second});
    }
    for (const auto& [id, c] : b)
        if (!a.count(id)) d.clips.push_back({std::nullopt, *c});
    return d;
}

void applyDiff(Project& p, const ProjectDiff& d, bool forward) {
    if (d.headerBefore) {
        const Project& h = forward ? *d.headerAfter : *d.headerBefore;
        // Runtime-only state (offline flags) belongs to "now", not to the history.
        std::map<Id, bool> offline;
        for (const auto& a : p.assets) offline[a.id] = a.offline;
        std::vector<Clip> clips = std::move(p.sequence.clips);
        p.name = h.name;
        p.settings = h.settings;
        p.bins = h.bins;
        p.assets = h.assets;
        p.nextAssetId = h.nextAssetId;
        p.otherSequences = h.otherSequences;
        p.activeSequence = h.activeSequence;
        p.nextSequenceUid = h.nextSequenceUid;
        Sequence s = h.sequence;
        s.clips = std::move(clips);
        p.sequence = std::move(s);
        for (auto& a : p.assets) {
            auto it = offline.find(a.id);
            if (it != offline.end()) a.offline = it->second;
        }
    }
    auto& clips = p.sequence.clips;
    for (const auto& [before, after] : d.clips) {
        const std::optional<Clip>& from = forward ? before : after;
        const std::optional<Clip>& to = forward ? after : before;
        Id id = from ? from->id : to->id;
        auto it = std::find_if(clips.begin(), clips.end(), [&](const Clip& c) { return c.id == id; });
        if (!to) {
            if (it != clips.end()) clips.erase(it);
        } else if (it != clips.end()) *it = *to;
        else clips.push_back(*to);
    }
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.id < b.id; });
}

void UndoStack::push(std::string label, ProjectDiff d) {
    entries_.resize(pos_);
    entries_.push_back({std::move(label), std::move(d)});
    pos_ = entries_.size();
    // Unlimited in steps, but bounded in memory (~512 MB) by dropping the oldest steps.
    while (entries_.size() > 1 && memoryBytes() > (size_t(512) << 20)) {
        entries_.erase(entries_.begin());
        --pos_;
    }
}

bool UndoStack::undo(Project& p) {
    if (!canUndo()) return false;
    --pos_;
    applyDiff(p, entries_[pos_].diff, false);
    return true;
}

bool UndoStack::redo(Project& p) {
    if (!canRedo()) return false;
    applyDiff(p, entries_[pos_].diff, true);
    ++pos_;
    return true;
}

size_t UndoStack::memoryBytes() const {
    size_t b = 0;
    for (const auto& e : entries_) b += e.diff.approxBytes();
    return b;
}

void Document::begin(const std::string& label) {
    if (snapshot_) return;  // nested: the outer transaction wins
    snapshot_ = project;
    label_ = label;
}

bool Document::commit() {
    if (!snapshot_) return false;
    edit::normalize(project.sequence);
    ProjectDiff d = makeDiff(*snapshot_, project);
    snapshot_.reset();
    if (d.empty()) return false;
    undo.push(label_, std::move(d));
    ++revision_;
    return true;
}

void Document::rollback() {
    if (!snapshot_) return;
    project = std::move(*snapshot_);
    snapshot_.reset();
    ++revision_;
}

bool Document::doUndo() {
    if (snapshot_) rollback();
    if (!undo.undo(project)) return false;
    ++revision_;
    return true;
}

bool Document::doRedo() {
    if (snapshot_) rollback();
    if (!undo.redo(project)) return false;
    ++revision_;
    return true;
}

void Document::reset(Project p) {
    snapshot_.reset();
    project = std::move(p);
    undo.clear();
    ++revision_;
    savedRevision_ = revision_;
}

} // namespace cut
