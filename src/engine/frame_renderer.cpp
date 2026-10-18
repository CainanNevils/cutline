#include "engine/frame_renderer.h"

#include "core/lut.h"
#include "render/text_raster.h"

#include <algorithm>
#include <cmath>

namespace cut {

MotionValues motionAt(const Clip& c, Tick local) {
    MotionValues m;
    const Effect& e = c.motion;
    m.posX = e.p("posX", local);
    m.posY = e.p("posY", local);
    m.scale = e.p("scale", local);
    m.scaleW = e.p("scaleW", local);
    m.uniform = e.p("uniform", local) > 0.5;
    m.rotation = e.p("rotation", local);
    m.anchorX = e.p("anchorX", local);
    m.anchorY = e.p("anchorY", local);
    return m;
}

double clipOpacityAt(const Clip& c, Tick local) {
    double o = c.opacity.type.empty() ? 1.0 : c.opacity.p("opacity", local) / 100.0;
    if (c.kind == ClipKind::Text || c.kind == ClipKind::Matte) {
        if (c.fadeIn > 0 && local < c.fadeIn) o *= std::clamp(double(local) / double(c.fadeIn), 0.0, 1.0);
        if (c.fadeOut > 0 && local > c.duration - c.fadeOut)
            o *= std::clamp(double(c.duration - local) / double(c.fadeOut), 0.0, 1.0);
    }
    return std::clamp(o, 0.0, 1.0);
}

bool clipSourceSize(const RenderSnapshot& snap, const Clip& c, int& w, int& h) {
    if (c.kind == ClipKind::Text) return false;
    if (c.kind == ClipKind::Matte || c.kind == ClipKind::Adjustment) {  // full frame
        w = snap.seq.settings.width;
        h = snap.seq.settings.height;
        return true;
    }
    if (c.kind == ClipKind::Nested) {
        auto it = snap.nested.find(c.nestedSeq);
        if (it == snap.nested.end()) return false;
        w = it->second->seq.settings.width;
        h = it->second->seq.settings.height;
        return w > 0 && h > 0;
    }
    const AssetRef* a = snap.asset(c.assetId);
    if (!a || !a->info.hasVideo) return false;
    w = a->info.video.width;
    h = a->info.video.height;
    return w > 0 && h > 0;
}

GpuTexturePtr FrameRenderer::temp(int idx, const GpuTexture& like) {
    GpuTexturePtr& t = temps_[std::clamp(depth_, 0, 3)][idx];
    if (!t || t->width != like.width || t->height != like.height || t->format != like.format)
        t = comp_.gpu().acquire(like.width, like.height, like.format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    return t;
}

GpuTexturePtr FrameRenderer::textTexture(const std::string& text, const TextStyle& style, int& w, int& h, float& dx,
                                         float& dy, int visibleChars) {
    uint64_t key = textHash(text, style, visibleChars);
    auto it = text_.find(key);
    if (it == text_.end()) {
        TextImage img = rasterizeText(text, style, 1.0, visibleChars);
        TextEntry e;
        e.tex = comp_.uploadBGRA(img.width, img.height, img.bgra.data(), img.width * 4);
        e.w = img.width;
        e.h = img.height;
        e.dx = img.centerDx;
        e.dy = img.centerDy;
        it = text_.emplace(key, std::move(e)).first;
    }
    it->second.lastUse = frameCounter_;
    w = it->second.w;
    h = it->second.h;
    dx = it->second.dx;
    dy = it->second.dy;
    return it->second.tex;
}

ID3D11ShaderResourceView* FrameRenderer::lut(const std::string& path, int& size) {
    if (path.empty()) return nullptr;
    auto it = luts_.find(path);
    if (it == luts_.end()) {
        LutEntry e;
        Lut3D l;
        std::string err;
        if (loadCubeFile(path, l, err)) {
            e.srv = comp_.createLut(l);
            e.size = l.size;
        }
        e.failed = !e.srv;
        it = luts_.emplace(path, std::move(e)).first;
    }
    size = it->second.size;
    return it->second.srv.Get();
}

void FrameRenderer::trimCaches() {
    for (auto it = text_.begin(); it != text_.end();) {
        if (frameCounter_ - it->second.lastUse > 600) it = text_.erase(it);
        else ++it;
    }
}

bool FrameRenderer::clipCorners(const RenderSnapshot& snap, const Clip& c, Tick t, Corners& out) {
    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
    int sw = 0, sh = 0;
    if (c.kind == ClipKind::Text) {
        TextImage img = rasterizeText(c.text, c.textStyle, 1.0);
        sw = img.width;
        sh = img.height;
    } else if (!clipSourceSize(snap, c, sw, sh)) return false;
    out = computeCorners(sw, sh, W, H, c.fit, motionAt(c, t - c.start));
    return true;
}

namespace {
// Gives the clips of a nested sequence keys of their own (see nestedClipKey).
class NestedProvider : public FrameProvider {
public:
    NestedProvider(FrameProvider& base, Id nestClip) : base_(base), nest_(nestClip) {}
    GpuFramePtr frame(const Clip& c, const AssetRef& a, Tick t, bool& late) override {
        Clip x = c;
        x.id = nestedClipKey(nest_, c.id);
        return base_.frame(x, a, t, late);
    }
    GpuFramePtr nextFrame(const Clip& c, const AssetRef& a, const GpuFramePtr& f) override {
        Clip x = c;
        x.id = nestedClipKey(nest_, c.id);
        return base_.nextFrame(x, a, f);
    }

private:
    FrameProvider& base_;
    Id nest_;
};
} // namespace

GpuTexturePtr FrameRenderer::matteTexture(uint32_t argb) {
    auto it = mattes_.find(argb);
    if (it != mattes_.end()) return it->second;
    const uint8_t a = uint8_t(argb >> 24);
    auto pm = [&](int shift) { return uint8_t(((argb >> shift) & 0xFF) * a / 255); };
    uint8_t px[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) {
        px[i * 4 + 0] = pm(0);   // B
        px[i * 4 + 1] = pm(8);   // G
        px[i * 4 + 2] = pm(16);  // R
        px[i * 4 + 3] = a;
    }
    if (mattes_.size() > 64) mattes_.clear();
    return mattes_[argb] = comp_.uploadBGRA(4, 4, px, 16);
}

void FrameRenderer::applyAdjustment(const RenderSnapshot& snap, const LayerClip& lc, const GpuTexture& canvas,
                                    FrameProvider& provider, RenderStats& st) {
    // the picture so far goes back onto the canvas through the layer's motion and effects;
    // opacity mixes the adjusted and the untouched picture (both added onto a cleared canvas)
    GpuTexturePtr src = temp(2, canvas);
    if (!src) return;
    comp_.copy(*src, canvas);
    comp_.clear(canvas);
    drawClip(snap, lc, canvas, provider, st, src.get());
}

void FrameRenderer::drawClip(const RenderSnapshot& snap, const LayerClip& lc, const GpuTexture& target,
                             FrameProvider& provider, RenderStats& st, const GpuTexture* adjust) {
    const Clip& c = *lc.clip;
    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
    LayerParams lp;
    GpuTexturePtr textTex;
    GpuFramePtr frame, frame2;
    int sw = 0, sh = 0;
    MotionValues mv = motionAt(c, lc.local);
    if (c.kind == ClipKind::Adjustment) {
        if (!adjust) return;  // (inside a transition there is no picture to adjust)
        lp.rgba = adjust;
        lp.rgbaPremul = true;
        sw = lp.logicalW = W;
        sh = lp.logicalH = H;
    } else if (c.kind == ClipKind::Matte) {
        textTex = matteTexture(c.matteColor);
        if (!textTex) return;
        lp.rgba = textTex.get();
        lp.rgbaPremul = true;
        sw = lp.logicalW = W;
        sh = lp.logicalH = H;
    } else if (c.kind == ClipKind::Text) {
        float dx = 0, dy = 0;
        int visible = -1;  // Typewriter: characters revealed so far (line breaks not counted)
        for (const auto& e : c.effects)
            if (e.type == fx::Typewriter && e.enabled) {
                const double p = std::clamp(e.p("progress", lc.local) / 100.0, 0.0, 1.0);
                if (p < 1.0) {
                    int n = 0;  // UTF-16 units (what the rasterizer counts), line breaks excluded
                    for (unsigned char ch : c.text)
                        if ((ch & 0xC0) != 0x80 && ch != '\n') n += (ch & 0xF8) == 0xF0 ? 2 : 1;
                    visible = int(std::floor(p * n + 1e-6));
                }
            }
        textTex = textTexture(c.text, c.textStyle, sw, sh, dx, dy, visible);
        if (!textTex) return;
        lp.rgba = textTex.get();
        lp.rgbaPremul = true;
    } else if (c.kind == ClipKind::Nested) {
        // render the nested sequence at the clip's source time into its own canvas, then treat
        // that canvas like any other layer (motion, opacity and effects apply to the whole nest)
        auto it = snap.nested.find(c.nestedSeq);
        if (it == snap.nested.end() || depth_ >= 3) {
            ++st.missing;
            return;
        }
        const RenderSnapshot& ns = *it->second;
        sw = ns.seq.settings.width;
        sh = ns.seq.settings.height;
        // same scale as the target (preview quality), same precision
        const double scale = double(target.width) / double(std::max(1, W));
        const int cw = std::max(2, int(sw * scale)), ch = std::max(2, int(sh * scale));
        GpuTexturePtr& nc = nestCanvas_[depth_];
        if (!nc || nc->width != cw || nc->height != ch || nc->format != target.format)
            nc = comp_.makeCanvas(cw, ch, target.format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (!nc) return;
        textTex = nc;  // keeps it alive for this draw
        NestedProvider np(provider, c.id);
        ++depth_;
        RenderStats inner = render(ns, std::max<Tick>(0, lc.source), *nc, np, true);
        --depth_;
        st.late += inner.late;
        st.missing += inner.missing;
        lp.rgba = nc.get();
        lp.rgbaPremul = true;
        lp.logicalW = sw;
        lp.logicalH = sh;
    } else {
        const AssetRef* a = snap.asset(c.assetId);
        if (!a || a->offline || !a->info.hasVideo) {
            ++st.missing;
            return;
        }
        Tick src = lc.source;
        if (!a->info.isImage && a->info.duration > 0) src = std::clamp<Tick>(src, 0, a->info.duration - 1);
        bool late = false;
        frame = provider.frame(c, *a, src, late);
        if (!frame) {
            ++st.missing;
            return;
        }
        if (late) ++st.late;
        if (c.frameBlend && !c.isHold() && c.speed < 1.0) {
            frame2 = provider.nextFrame(c, *a, frame);
            Tick end = frame->end.load();
            if (frame2 && end > frame->pts) {
                lp.frame2 = frame2.get();
                lp.blend2 = float(std::clamp(double(src - frame->pts) / double(end - frame->pts), 0.0, 1.0));
            }
        }
        lp.frame = frame.get();
        sw = a->info.video.width > 0 ? a->info.video.width : frame->width;
        sh = a->info.video.height > 0 ? a->info.video.height : frame->height;
    }
    lp.corners = computeCorners(sw, sh, W, H, c.fit, mv);
    lp.opacity = float(clipOpacityAt(c, lc.local));
    lp.blend = BlendMode(int(std::clamp(c.opacity.type.empty() ? 0.0 : c.opacity.p("blend", 0), 0.0, 3.0)));
    bool sawCc = false;
    for (const Effect& e : c.effects) {
        if (!e.enabled) continue;
        if (e.type == fx::Crop) {
            lp.crop[0] = float(std::clamp(e.p("left", lc.local) / 100.0, 0.0, 1.0));
            lp.crop[1] = float(std::clamp(e.p("top", lc.local) / 100.0, 0.0, 1.0));
            lp.crop[2] = float(std::clamp(e.p("right", lc.local) / 100.0, 0.0, 1.0));
            lp.crop[3] = float(std::clamp(e.p("bottom", lc.local) / 100.0, 0.0, 1.0));
        } else if (e.type == fx::Color) {
            sawCc = true;
            lp.cc.enabled = true;
            lp.cc.exposure = float(e.p("exposure", lc.local));
            lp.cc.contrast = float(e.p("contrast", lc.local) / 100.0);
            lp.cc.highlights = float(e.p("highlights", lc.local) / 100.0);
            lp.cc.shadows = float(e.p("shadows", lc.local) / 100.0);
            lp.cc.saturation = float(e.p("saturation", lc.local) / 100.0);
            lp.cc.temperature = float(e.p("temperature", lc.local) / 100.0);
            lp.cc.tint = float(e.p("tint", lc.local) / 100.0);
        } else if (e.type == fx::Lut) {
            int size = 0;
            ID3D11ShaderResourceView* srv = lut(e.path, size);
            if (srv) {
                lp.lut = srv;
                lp.lutSize = size;
                lp.lutIntensity = float(std::clamp(e.p("intensity", lc.local) / 100.0, 0.0, 1.0));
                lp.lutFirst = !sawCc;
            }
        } else if (e.type == fx::Flip) {
            lp.flipH ^= e.p("horizontal", 0) >= 0.5;
            lp.flipV ^= e.p("vertical", 0) >= 0.5;
        } else if (e.type == fx::Blur) {
            lp.blurPx = float(std::clamp(e.p("radius", lc.local), 0.0, 100.0));
        } else if (e.type == fx::Sharpen) {
            lp.sharpen = float(std::clamp(e.p("amount", lc.local) / 100.0, 0.0, 2.0));
        } else if (e.type == fx::BlackWhite) {
            lp.blackWhite = float(std::clamp(e.p("amount", lc.local) / 100.0, 0.0, 1.0));
        } else if (e.type == fx::Mosaic) {
            lp.mosaic = float(std::clamp(e.p("size", lc.local), 2.0, 400.0));
        } else if (e.type == fx::Glow) {
            lp.glow[0] = float(std::clamp(e.p("intensity", lc.local) / 100.0, 0.0, 3.0));
            lp.glow[1] = float(std::clamp(e.p("threshold", lc.local) / 100.0, 0.0, 0.99));
            lp.glow[2] = float(std::clamp(e.p("radius", lc.local), 1.0, 100.0));
        } else if (e.type == fx::ChromaKey) {
            lp.keyColor = e.p("key", 0) >= 0.5 ? 2 : 1;
            lp.keyTolerance = float(std::clamp(e.p("tolerance", lc.local) / 100.0, 0.0, 1.0));
            lp.keySoftness = float(std::clamp(e.p("softness", lc.local) / 100.0, 0.0, 1.0));
            lp.keySpill = float(std::clamp(e.p("spill", lc.local) / 100.0, 0.0, 1.0));
        } else if (e.type == fx::Mask) {
            lp.maskShape = e.p("shape", 0) >= 0.5 ? 2 : 1;
            lp.mask[0] = float(e.p("cx", lc.local) / 100.0);
            lp.mask[1] = float(e.p("cy", lc.local) / 100.0);
            lp.mask[2] = float(std::max(0.0, e.p("w", lc.local) / 100.0));
            lp.mask[3] = float(std::max(0.0, e.p("h", lc.local) / 100.0));
            lp.maskFeather = float(std::max(0.0, e.p("feather", lc.local)));
            lp.maskInvert = e.p("invert", 0) >= 0.5;
        } else if (e.type == fx::DropShadow) {
            const double dir = e.p("direction", lc.local) * 3.14159265358979 / 180.0, dist = e.p("distance", lc.local);
            lp.shadowOpacity = float(std::clamp(e.p("opacity", lc.local) / 100.0, 0.0, 1.0));
            lp.shadowDx = float(std::sin(dir) * dist);   // 0 deg = up, 90 = right, 135 = down-right
            lp.shadowDy = float(-std::cos(dir) * dist);
            lp.shadowSoft = float(std::max(0.0, e.p("softness", lc.local)));
        } else if (e.type == fx::Rounded) {
            static const float colors[8][3] = {{1, 1, 1}, {0, 0, 0}, {0.18f, 0.55f, 0.92f}, {0.25f, 0.73f, 0.31f},
                                               {0.90f, 0.33f, 0.29f}, {0.89f, 0.57f, 0.24f}, {0.91f, 0.77f, 0.28f}, {0.69f, 0.42f, 0.85f}};
            lp.cornerRadius = float(std::max(0.0, e.p("radius", lc.local)));
            lp.borderWidth = float(std::max(0.0, e.p("border", lc.local)));
            const int ci = std::clamp(int(e.p("color", 0)), 0, 7);
            for (int k = 0; k < 3; ++k) lp.borderColor[k] = colors[ci][k];
        } else if (e.type == fx::Vignette) {
            lp.vignette[0] = float(std::clamp(e.p("amount", lc.local) / 100.0, -1.0, 1.0));
            lp.vignette[1] = float(std::clamp(e.p("midpoint", lc.local) / 100.0, 0.0, 1.0));
            lp.vignette[2] = float(std::clamp(e.p("feather", lc.local) / 100.0, 0.01, 1.0));
        }
    }
    if (adjust) {
        // the untouched picture, then the adjusted one over it through the layer's opacity, blend
        // mode, mask and crop - so a masked adjustment layer changes only the masked area
        LayerParams plain;
        plain.rgba = adjust;
        plain.rgbaPremul = true;
        plain.corners = computeCorners(W, H, W, H, FitMode::Fit, MotionValues{});
        comp_.drawLayer(target, W, H, plain);
        lp.opacity = std::clamp(lp.opacity, 0.f, 1.f);
        if (lp.opacity > 0) comp_.drawLayer(target, W, H, lp);
        ++st.layers;
        return;
    }
    if (lp.opacity <= 0.0f) return;
    comp_.drawLayer(target, W, H, lp);
    ++st.layers;
}

RenderStats FrameRenderer::render(const RenderSnapshot& snap, Tick t, const GpuTexture& canvas, FrameProvider& provider,
                                  bool drawCaptions) {
    ++frameCounter_;
    RenderStats st;
    comp_.clear(canvas);
    auto layers = evaluateVideo(snap.seq, t);
    for (const VideoLayer& L : layers) {
        if (!L.transition) {
            if (L.a.clip && L.a.clip->kind == ClipKind::Adjustment) applyAdjustment(snap, L.a, canvas, provider, st);
            else drawClip(snap, L.a, canvas, provider, st);
            continue;
        }
        GpuTexturePtr ta = temp(0, canvas), tb = temp(1, canvas);
        if (!ta || !tb) continue;
        comp_.clear(*ta);
        comp_.clear(*tb);
        if (L.a.clip) drawClip(snap, L.a, *ta, provider, st);
        if (L.b.clip) drawClip(snap, L.b, *tb, provider, st);
        comp_.drawTransition(canvas, *ta, *tb, L.transition->type, float(L.progress));
    }
    if (drawCaptions && snap.showCaptions) {
        if (const Caption* cap = captionAt(snap.seq, t)) {
            if (!cap->text.empty()) {
                int w = 0, h = 0;
                float dx = 0, dy = 0;
                const CaptionStyle& cs = snap.seq.captionStyle;
                GpuTexturePtr tex = textTexture(cap->text, cs.text, w, h, dx, dy);
                if (tex) {
                    const int W = snap.seq.settings.width, H = snap.seq.settings.height;
                    MotionValues mv;
                    mv.posY = H / 2.0 - cs.marginBottom / 100.0 * H - h / 2.0;
                    LayerParams lp;
                    lp.rgba = tex.get();
                    lp.corners = computeCorners(w, h, W, H, FitMode::None, mv);
                    comp_.drawLayer(canvas, W, H, lp);
                }
            }
        }
    }
    if ((frameCounter_ & 255) == 0) trimCaches();
    return st;
}

} // namespace cut
