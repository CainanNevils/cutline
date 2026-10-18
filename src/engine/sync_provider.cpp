#include "engine/sync_provider.h"

namespace cut {

FrameReader* SyncFrameProvider::reader(const Clip& c, const AssetRef& a) {
    auto it = readers_.find(c.id);
    if (it != readers_.end()) return it->second.get();
    if (failed_.count(c.id)) return nullptr;
    auto r = std::make_unique<FrameReader>();
    VideoDecoder::Options o;
    o.allowHw = allowHw_;
    std::string err;
    if (!r->open(a.path, gpu_, a.info, o, err)) {
        err_ = a.path + ": " + err;
        failed_.insert(c.id);
        return nullptr;
    }
    r->setWindow(c.reverse ? 32 : 6);
    return readers_.emplace(c.id, std::move(r)).first->second.get();
}

GpuFramePtr SyncFrameProvider::frame(const Clip& c, const AssetRef& a, Tick src, bool& late) {
    late = false;
    FrameReader* r = reader(c, a);
    return r ? r->get(src) : nullptr;
}

GpuFramePtr SyncFrameProvider::nextFrame(const Clip& c, const AssetRef& a, const GpuFramePtr& f) {
    FrameReader* r = reader(c, a);
    if (!r || !f) return nullptr;
    if (GpuFramePtr n = r->nextAfter(f)) return n;
    GpuFramePtr n = r->get(f->end.load());
    return n && n->pts > f->pts ? n : nullptr;
}

void SyncFrameProvider::retain(const std::set<Id>& keep) {
    // readers of clips inside a nest live as long as the nest clip itself
    std::set<Id> nests;
    for (Id id : keep) nests.insert(id & 0xFFFFFF);
    for (auto it = readers_.begin(); it != readers_.end();) {
        const bool nestedKey = (it->first & kNestedClipKeyFlag) != 0;
        const bool live = nestedKey ? nests.count((it->first >> 32) & 0xFFFFFF) > 0 : keep.count(it->first) > 0;
        if (!live) it = readers_.erase(it);
        else ++it;
    }
}

bool SyncFrameProvider::anySoftware() const {
    for (const auto& [id, r] : readers_)
        if (!r->isHw()) return true;
    return false;
}

} // namespace cut
