#pragma once
// Undo/redo. Every edit runs as a transaction on the Document: the project is snapshotted at
// begin(), the edit mutates it freely (a whole mouse drag may live in one transaction), and
// commit() stores only the difference (changed clips by id + the small non-clip "header" when it
// changed). This gives command-pattern undo for every operation without per-command inverse code.
#include "core/model.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cut {

struct ProjectDiff {
    std::optional<Project> headerBefore, headerAfter;  // project with sequence.clips emptied
    std::vector<std::pair<std::optional<Clip>, std::optional<Clip>>> clips;  // before, after
    bool empty() const { return !headerBefore && clips.empty(); }
    size_t approxBytes() const;
};

ProjectDiff makeDiff(const Project& before, const Project& after);
void applyDiff(Project& p, const ProjectDiff& d, bool forward);

class UndoStack {
public:
    void push(std::string label, ProjectDiff d);
    bool canUndo() const { return pos_ > 0; }
    bool canRedo() const { return pos_ < entries_.size(); }
    std::string undoLabel() const { return canUndo() ? entries_[pos_ - 1].label : std::string(); }
    std::string redoLabel() const { return canRedo() ? entries_[pos_].label : std::string(); }
    bool undo(Project& p);
    bool redo(Project& p);
    void clear() {
        entries_.clear();
        pos_ = 0;
    }
    size_t size() const { return entries_.size(); }
    size_t position() const { return pos_; }
    const std::string& label(size_t i) const { return entries_[i].label; }
    size_t memoryBytes() const;

private:
    struct Entry {
        std::string label;
        ProjectDiff diff;
    };
    std::vector<Entry> entries_;
    size_t pos_ = 0;
};

class Document {
public:
    Project project;
    UndoStack undo;

    void begin(const std::string& label);
    // Returns true if the transaction changed anything (and pushed an undo step).
    bool commit();
    void rollback();
    bool inTransaction() const { return snapshot_.has_value(); }
    // Restore the state captured at begin() without ending the transaction (drags re-apply the
    // whole gesture from the start state on every mouse move).
    void revertToSnapshot() {
        if (snapshot_) project = *snapshot_;
    }
    // Mark live (uncommitted) change inside a transaction so viewers refresh.
    void touch() { ++revision_; }

    template <class F>
    bool edit(const std::string& label, F&& f) {
        begin(label);
        f(project);
        return commit();
    }

    bool doUndo();
    bool doRedo();

    uint64_t revision() const { return revision_; }
    bool dirty() const { return revision_ != savedRevision_; }
    void markSaved() { savedRevision_ = revision_; }
    void reset(Project p);

private:
    std::optional<Project> snapshot_;
    std::string label_;
    uint64_t revision_ = 1, savedRevision_ = 1;
};

} // namespace cut
