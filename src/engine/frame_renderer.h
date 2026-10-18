#pragma once
// Renders one timeline frame (all video tracks, transitions, titles, burned captions) onto a
// canvas. Shared by preview (non-blocking provider that may hold late frames) and export
// (blocking, frame-exact provider), so both produce the same picture.
#include "core/evaluate.h"
#include "engine/snapshot.h"
#include "render/compositor.h"

#include <list>
#include <unordered_map>

namespace cut {

class FrameProvider {
public:
    virtual ~FrameProvider() = default;
    // Frame of `asset` shown at media time `sourceTime` for `clip`. `late` is set when a held /
    // approximate frame is returned because the exact one is not decoded yet (preview only).
    virtual GpuFramePtr frame(const Clip& clip, const AssetRef& asset, Tick sourceTime, bool& late) = 0;
    virtual GpuFramePtr nextFrame(const Clip& clip, const AssetRef& asset, const GpuFramePtr& f) = 0;
};

// Clip ids inside a nested sequence are only unique within it: providers see them as
// (nest clip id, inner id) mixed into one key with this bit set.
constexpr Id kNestedClipKeyFlag = Id(1) << 62;
inline Id nestedClipKey(Id nestClip, Id inner) { return kNestedClipKeyFlag | ((nestClip & 0xFFFFFF) << 32) | (inner & 0xFFFFFFFF); }

struct RenderStats {
    int layers = 0;
    int late = 0;     // layers shown with a held frame
    int missing = 0;  // layers with no frame at all
    bool missingHandles = false;
};

class FrameRenderer {
public:
    explicit FrameRenderer(Compositor& c) : comp_(c) {}
    // canvas: RGBA canvas (any size; content is scaled from sequence coordinates).
    RenderStats render(const RenderSnapshot& snap, Tick t, const GpuTexture& canvas, FrameProvider& provider,
                       bool drawCaptions);
    void trimCaches();
    // Selected-clip outline for on-canvas handles (sequence pixel corners). False if not visible.
    static bool clipCorners(const RenderSnapshot& snap, const Clip& c, Tick t, Corners& out);

private:
    // adjust: for adjustment layers, a copy of the picture so far (drawn back through the effects)
    void drawClip(const RenderSnapshot& snap, const LayerClip& lc, const GpuTexture& target, FrameProvider& provider,
                  RenderStats& st, const GpuTexture* adjust = nullptr);
    void applyAdjustment(const RenderSnapshot& snap, const LayerClip& lc, const GpuTexture& canvas, FrameProvider& provider,
                         RenderStats& st);
    GpuTexturePtr matteTexture(uint32_t argb);
    GpuTexturePtr textTexture(const std::string& text, const TextStyle& style, int& w, int& h, float& dx, float& dy,
                              int visibleChars = -1);
    ID3D11ShaderResourceView* lut(const std::string& path, int& size);
    GpuTexturePtr temp(int idx, const GpuTexture& like);

    Compositor& comp_;
    struct TextEntry {
        GpuTexturePtr tex;
        int w = 0, h = 0;
        float dx = 0, dy = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<uint64_t, TextEntry> text_;
    struct LutEntry {
        ComPtr<ID3D11ShaderResourceView> srv;
        int size = 0;
        bool failed = false;
    };
    std::unordered_map<std::string, LutEntry> luts_;
    GpuTexturePtr temps_[4][3];  // per nesting depth
    GpuTexturePtr nestCanvas_[4];
    std::unordered_map<uint32_t, GpuTexturePtr> mattes_;
    int depth_ = 0;
    uint64_t frameCounter_ = 0;
};

// Helpers shared with the UI (Effect Controls defaults, handle hit testing).
MotionValues motionAt(const Clip& c, Tick local);
double clipOpacityAt(const Clip& c, Tick local);  // includes title fades
// Source pixel size of a clip (video frame size; nested: that sequence's frame size).
bool clipSourceSize(const RenderSnapshot& snap, const Clip& c, int& w, int& h);

} // namespace cut
