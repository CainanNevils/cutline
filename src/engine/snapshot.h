#pragma once
// Immutable view of the project handed from the UI thread to the render/mixer/export threads.
// The UI publishes a new snapshot after every edit (copy of the sequence + asset table); worker
// threads only ever read snapshots, so no locking of the live model is needed.
#include "core/model.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace cut {

struct AssetRef {
    Id id = 0;
    std::string path;
    std::string proxyPath;  // non-empty when a proxy is ready (used only if proxies are enabled)
    MediaInfo info;
    MediaInfo proxyInfo;
    bool offline = false;
};

struct RenderSnapshot {
    Sequence seq;
    // sequences shown by nested clips (by Sequence::uid), each with its own nested map
    std::unordered_map<Id, std::shared_ptr<const RenderSnapshot>> nested;
    std::unordered_map<Id, AssetRef> assets;
    uint64_t revision = 0;
    bool useProxies = false;   // preview only; export ignores proxies
    bool showCaptions = true;

    const AssetRef* asset(Id id) const {
        auto it = assets.find(id);
        return it == assets.end() ? nullptr : &it->second;
    }
};
using SnapshotPtr = std::shared_ptr<const RenderSnapshot>;

// Fills s.nested with the project's sequences that nested clips in s.seq show (recursively,
// stopping at cycles and at depth 3). s.assets / flags must be set first; children copy them.
void addNestedSnapshots(RenderSnapshot& s, const Project& p);

} // namespace cut
