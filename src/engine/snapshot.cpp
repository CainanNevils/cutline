#include "engine/snapshot.h"

#include <functional>
#include <set>

namespace cut {

void addNestedSnapshots(RenderSnapshot& root, const Project& p) {
    std::set<Id> path{root.seq.uid};
    std::function<void(RenderSnapshot&, int)> add = [&](RenderSnapshot& rs, int depth) {
        if (depth >= 3) return;
        for (const auto& c : rs.seq.clips) {
            if (c.kind != ClipKind::Nested || rs.nested.count(c.nestedSeq) || path.count(c.nestedSeq)) continue;
            const Sequence* ns = p.sequenceByUid(c.nestedSeq);
            if (!ns) continue;
            auto child = std::make_shared<RenderSnapshot>();
            child->seq = *ns;
            child->assets = root.assets;
            child->revision = root.revision;
            child->useProxies = root.useProxies;
            child->showCaptions = root.showCaptions;
            path.insert(c.nestedSeq);
            add(*child, depth + 1);
            path.erase(c.nestedSeq);
            rs.nested[c.nestedSeq] = std::move(child);
        }
    };
    add(root, 0);
}

} // namespace cut