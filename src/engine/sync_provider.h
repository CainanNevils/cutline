#pragma once
// Blocking, frame-exact provider: export, proxy-free renders, rendertest. Always reads the
// original media (never proxies). One FrameReader per clip so sequential exports decode each
// clip forward without seeking; reversed clips use the bounded reverse window.
#include "engine/frame_renderer.h"
#include "media/frame_reader.h"

#include <map>
#include <memory>
#include <set>

namespace cut {

class SyncFrameProvider : public FrameProvider {
public:
    SyncFrameProvider(std::shared_ptr<GpuDevice> gpu, bool allowHw) : gpu_(std::move(gpu)), allowHw_(allowHw) {}
    GpuFramePtr frame(const Clip& c, const AssetRef& a, Tick src, bool& late) override;
    GpuFramePtr nextFrame(const Clip& c, const AssetRef& a, const GpuFramePtr& f) override;
    // Close readers of clips that are no longer needed (ids not in `keep`).
    void retain(const std::set<Id>& keep);
    int readers() const { return int(readers_.size()); }
    bool anySoftware() const;
    std::string lastError() const { return err_; }

private:
    FrameReader* reader(const Clip& c, const AssetRef& a);
    std::shared_ptr<GpuDevice> gpu_;
    bool allowHw_;
    std::map<Id, std::unique_ptr<FrameReader>> readers_;
    std::set<Id> failed_;
    std::string err_;
};

} // namespace cut
