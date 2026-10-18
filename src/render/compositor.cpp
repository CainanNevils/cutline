#include "render/compositor.h"

#include <cmath>
#include <cstring>

#include "layer_ps.h"
#include "layer_vs.h"
#include "present_blit_ps.h"
#include "present_blit_vs.h"
#include "present_chroma_ps.h"
#include "present_luma_ps.h"
#include "present_rect_ps.h"
#include "present_rect_vs.h"
#include "transition_ps.h"
#include "transition_vs.h"

namespace cut {

namespace {
struct alignas(16) LayerCB {
    float corners[4][4];
    float cropRect[4];
    float yuv[3][4];
    float yuvOffset[4];
    int srcMode;
    int transfer;
    float opacity;
    float blend2;
    float cc0[4];
    float cc1[4];
    float lutP[4];
    float fx0[4];  // flipH, flipV, black & white, sharpen
    float fx1[4];  // vignette amount, midpoint, feather, -
    float fx2[4];  // blur radius (uv x, y), texel (uv x, y)
    float fx3[4];  // visible uv extent (u, v) of the texture, corner radius, border width (px)
    float fx4[4];  // border colour (rgb), shadow opacity
    float fx5[4];  // shadow offset (px x, y), softness (px)
    float fx6[4];  // chroma key: colour (0 off), tolerance, softness, spill
    float fx7[4];  // mask centre (x, y), size (w, h) as fractions of the image
    float fx8[4];  // mask shape (0 off), feather px, invert
    float fx9[4];  // glow intensity, threshold, radius uv (x, y)
};
struct alignas(16) TransitionCB {
    int type;
    float progress;
    float texel[2];
};
struct alignas(16) BlitCB {
    float srcRect[4];
    float color[4];
    float dstRect[4];
    float params[4];
};

template <class T>
void upload(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const T& v) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &v, sizeof(T));
        ctx->Unmap(b, 0);
    }
}

ComPtr<ID3D11Buffer> makeCB(ID3D11Device* d, UINT size) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (size + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> b;
    d->CreateBuffer(&bd, nullptr, &b);
    return b;
}

ComPtr<ID3D11BlendState> makeBlend(ID3D11Device* d, bool enable, D3D11_BLEND src, D3D11_BLEND dst,
                                   D3D11_BLEND srcA = D3D11_BLEND_ONE, D3D11_BLEND dstA = D3D11_BLEND_INV_SRC_ALPHA) {
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.SrcBlend = src;
    rt.DestBlend = dst;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = srcA;
    rt.DestBlendAlpha = dstA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> s;
    d->CreateBlendState(&bd, &s);
    return s;
}

void yuvMatrix(PixelLayout layout, const ColorInfo& ci, float rows[3][4], float off[4]) {
    double kr = 0.2126, kb = 0.0722;
    if (ci.matrix == ColorMatrix::BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (ci.matrix == ColorMatrix::BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1 - kr - kb;
    double ys, cs, yo, co;
    const bool tenBit = layout == PixelLayout::P010;
    if (ci.range == ColorRange::Full) {
        ys = 1;
        cs = 1;
        yo = 0;
        co = tenBit ? 512.0 * 64 / 65535 : 128.0 / 255;
    } else if (tenBit) {
        ys = 65535.0 / ((940 - 64) * 64);
        cs = 65535.0 / ((960 - 64) * 64);
        yo = 64.0 * 64 / 65535;
        co = 512.0 * 64 / 65535;
    } else {
        ys = 255.0 / 219;
        cs = 255.0 / 224;
        yo = 16.0 / 255;
        co = 128.0 / 255;
    }
    const double rv = 2 * (1 - kr), bu = 2 * (1 - kb);
    const double gu = -2 * kb * (1 - kb) / kg, gv = -2 * kr * (1 - kr) / kg;
    float m[3][4] = {{float(ys), 0, float(rv * cs), 0},
                     {float(ys), float(gu * cs), float(gv * cs), 0},
                     {float(ys), float(bu * cs), 0, 0}};
    std::memcpy(rows, m, sizeof(m));
    off[0] = float(yo);
    off[1] = float(co);
    off[2] = float(co);
    off[3] = 0;
}
} // namespace

double fitScale(FitMode fit, int srcW, int srcH, int seqW, int seqH) {
    if (srcW <= 0 || srcH <= 0) return 1;
    double sx = double(seqW) / srcW, sy = double(seqH) / srcH;
    switch (fit) {
    case FitMode::Fit: return std::min(sx, sy);
    case FitMode::Fill: return std::max(sx, sy);
    case FitMode::None: return 1;
    }
    return 1;
}

Corners computeCorners(int srcW, int srcH, int seqW, int seqH, FitMode fit, const MotionValues& m) {
    const double base = fitScale(fit, srcW, srcH, seqW, seqH);
    const double sy = base * m.scale / 100.0;
    const double sx = base * (m.uniform ? m.scale : m.scaleW) / 100.0;
    const double th = m.rotation * 3.14159265358979323846 / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    const double cx = seqW / 2.0 + m.posX, cy = seqH / 2.0 + m.posY;
    Corners out{};
    const double uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        double x = ((uv[i][0] - 0.5) * srcW - m.anchorX) * sx;
        double y = ((uv[i][1] - 0.5) * srcH - m.anchorY) * sy;
        out.p[i][0] = float(cx + x * c - y * s);
        out.p[i][1] = float(cy + x * s + y * c);
    }
    return out;
}

bool Compositor::init(std::shared_ptr<GpuDevice> gpu, std::string& error) {
    gpu_ = std::move(gpu);
    ctx_ = gpu_->context();
    ID3D11Device* d = gpu_->device();
    HRESULT hr = S_OK;
    hr |= d->CreateVertexShader(layer_vs, sizeof(layer_vs), nullptr, &vsLayer_);
    hr |= d->CreatePixelShader(layer_ps, sizeof(layer_ps), nullptr, &psLayer_);
    hr |= d->CreateVertexShader(transition_vs, sizeof(transition_vs), nullptr, &vsFull_);
    hr |= d->CreatePixelShader(transition_ps, sizeof(transition_ps), nullptr, &psTransition_);
    hr |= d->CreateVertexShader(present_blit_vs, sizeof(present_blit_vs), nullptr, &vsBlit_);
    hr |= d->CreatePixelShader(present_blit_ps, sizeof(present_blit_ps), nullptr, &psBlit_);
    hr |= d->CreateVertexShader(present_rect_vs, sizeof(present_rect_vs), nullptr, &vsRect_);
    hr |= d->CreatePixelShader(present_rect_ps, sizeof(present_rect_ps), nullptr, &psRect_);
    hr |= d->CreatePixelShader(present_luma_ps, sizeof(present_luma_ps), nullptr, &psLuma_);
    hr |= d->CreatePixelShader(present_chroma_ps, sizeof(present_chroma_ps), nullptr, &psChroma_);
    if (FAILED(hr) || !psChroma_) {
        error = "Shader creation failed";
        return false;
    }
    cbLayer_ = makeCB(d, sizeof(LayerCB));
    cbTransition_ = makeCB(d, sizeof(TransitionCB));
    cbBlit_ = makeCB(d, sizeof(BlitCB));
    cbQuad_ = makeCB(d, 64);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    d->CreateSamplerState(&sd, &linear_);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    d->CreateSamplerState(&sd, &point_);
    blendNormal_ = makeBlend(d, true, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA);
    blendAdd_ = makeBlend(d, true, D3D11_BLEND_ONE, D3D11_BLEND_ONE);
    blendMultiply_ = makeBlend(d, true, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA);
    blendScreen_ = makeBlend(d, true, D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_ONE);
    blendOff_ = makeBlend(d, false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    d->CreateRasterizerState(&rd, &raster_);
    return true;
}

GpuTexturePtr Compositor::makeCanvas(int w, int h, bool hq) {
    return gpu_->acquire(w, h, hq ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
}

void Compositor::clear(const GpuTexture& t, float r, float g, float b, float a) {
    const float c[4] = {r, g, b, a};
    if (t.rtv0) ctx_->ClearRenderTargetView(t.rtv0.Get(), c);
}

void Compositor::setBlend(BlendMode m) {
    ID3D11BlendState* s = blendNormal_.Get();
    if (m == BlendMode::Add) s = blendAdd_.Get();
    else if (m == BlendMode::Multiply) s = blendMultiply_.Get();
    else if (m == BlendMode::Screen) s = blendScreen_.Get();
    ctx_->OMSetBlendState(s, nullptr, 0xffffffff);
}

static void setViewport(ID3D11DeviceContext* ctx, float x, float y, float w, float h) {
    D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
    ctx->RSSetViewports(1, &vp);
}

void Compositor::drawLayer(const GpuTexture& canvas, int seqW, int seqH, const LayerParams& p) {
    LayerCB cb{};
    float uvs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    float pos[4][2];
    for (int i = 0; i < 4; ++i) {
        pos[i][0] = p.corners.p[i][0];
        pos[i][1] = p.corners.p[i][1];
    }
    if (p.shadowOpacity > 0) {
        // grow the quad so the shadow can extend past the image (affine extrapolation)
        int vw = 1, vh = 1;
        if (p.frame) {
            vw = p.frame->width;
            vh = p.frame->height;
        } else if (p.rgba) {
            vw = p.rgba->width;
            vh = p.rgba->height;
        }
        if (p.logicalW > 0) {
            vw = p.logicalW;
            vh = p.logicalH;
        }
        const float e = std::abs(p.shadowDx) + std::abs(p.shadowDy) + p.shadowSoft + 2;
        const float du = e / float(std::max(1, vw)), dv = e / float(std::max(1, vh));
        const float ex[4][2] = {{-du, -dv}, {1 + du, -dv}, {-du, 1 + dv}, {1 + du, 1 + dv}};
        for (int i = 0; i < 4; ++i) {
            const float u = ex[i][0], v = ex[i][1];
            for (int k = 0; k < 2; ++k)
                pos[i][k] = p.corners.p[0][k] + (p.corners.p[1][k] - p.corners.p[0][k]) * u + (p.corners.p[2][k] - p.corners.p[0][k]) * v;
            uvs[i][0] = u;
            uvs[i][1] = v;
        }
    }
    for (int i = 0; i < 4; ++i) {
        cb.corners[i][0] = pos[i][0] / float(seqW) * 2.f - 1.f;
        cb.corners[i][1] = 1.f - pos[i][1] / float(seqH) * 2.f;
        cb.corners[i][2] = uvs[i][0];
        cb.corners[i][3] = uvs[i][1];
    }
    cb.cropRect[0] = p.crop[0];
    cb.cropRect[1] = p.crop[1];
    cb.cropRect[2] = 1.f - p.crop[2];
    cb.cropRect[3] = 1.f - p.crop[3];
    cb.opacity = p.opacity;
    ID3D11ShaderResourceView* srvs[5] = {};
    if (p.frame) {
        const GpuFrame& f = *p.frame;
        if (f.layout == PixelLayout::NV12 || f.layout == PixelLayout::P010) {
            cb.srcMode = 0;
            yuvMatrix(f.layout, f.color, cb.yuv, cb.yuvOffset);
            cb.transfer = int(f.color.transfer);
            srvs[0] = f.tex->srv0.Get();
            srvs[1] = f.tex->srv1.Get();
            // visible area of an even-padded texture
            float su = float(f.width) / float(f.tex->width), sv = float(f.height) / float(f.tex->height);
            for (int i = 0; i < 4; ++i) {
                cb.corners[i][2] *= su;
                cb.corners[i][3] *= sv;
            }
            cb.cropRect[0] *= su;
            cb.cropRect[2] *= su;
            cb.cropRect[1] *= sv;
            cb.cropRect[3] *= sv;
        } else {
            cb.srcMode = f.layout == PixelLayout::BGRAPremul ? 2 : 1;
            srvs[0] = f.tex->srv0.Get();
        }
        if (p.frame2 && p.blend2 > 0 && p.frame2->layout == f.layout && p.frame2->tex) {
            srvs[2] = p.frame2->tex->srv0.Get();
            srvs[3] = p.frame2->tex->srv1.Get();
            cb.blend2 = p.blend2;
        }
    } else if (p.rgba) {
        cb.srcMode = p.rgbaPremul ? 2 : 1;
        srvs[0] = p.rgba->srv0.Get();
    } else return;
    {
        // texture size for blur / sharpen offsets and the visible uv extent (padded NV12)
        int tw = 1, th = 1;
        float su = 1, sv = 1;
        if (p.frame && p.frame->tex) {
            tw = p.frame->tex->width;
            th = p.frame->tex->height;
            if (cb.srcMode == 0) {
                su = float(p.frame->width) / float(tw);
                sv = float(p.frame->height) / float(th);
            }
        } else if (p.rgba) {
            tw = p.rgba->width;
            th = p.rgba->height;
        }
        if (p.logicalW > 0 && p.logicalH > 0) {  // (visible extent stays 1 for these textures)
            tw = int(p.logicalW / su);
            th = int(p.logicalH / sv);
        }
        cb.fx0[0] = p.flipH ? 1.f : 0.f;
        cb.fx0[1] = p.flipV ? 1.f : 0.f;
        cb.fx0[2] = p.blackWhite;
        cb.fx0[3] = p.sharpen;
        cb.fx1[0] = p.vignette[0];
        cb.fx1[1] = p.vignette[1];
        cb.fx1[2] = p.vignette[2];
        cb.fx2[0] = p.blurPx / float(std::max(1, tw));
        cb.fx2[1] = p.blurPx / float(std::max(1, th));
        cb.fx2[2] = 1.f / float(std::max(1, tw));
        cb.fx2[3] = 1.f / float(std::max(1, th));
        cb.fx3[0] = su;
        cb.fx3[1] = sv;
        cb.fx3[2] = p.cornerRadius;
        cb.fx3[3] = p.borderWidth;
        cb.fx4[0] = p.borderColor[0];
        cb.fx4[1] = p.borderColor[1];
        cb.fx4[2] = p.borderColor[2];
        cb.fx4[3] = p.shadowOpacity;
        cb.fx5[0] = p.shadowDx;
        cb.fx5[1] = p.shadowDy;
        cb.fx5[2] = p.shadowSoft;
        cb.fx6[0] = float(p.keyColor);
        cb.fx6[1] = p.keyTolerance;
        cb.fx6[2] = p.keySoftness;
        cb.fx6[3] = p.keySpill;
        for (int k = 0; k < 4; ++k) cb.fx7[k] = p.mask[k];
        cb.fx8[0] = float(p.maskShape);
        cb.fx8[1] = p.maskFeather;
        cb.fx8[2] = p.maskInvert ? 1.f : 0.f;
        cb.fx8[3] = p.mosaic;
        cb.fx9[0] = p.glow[0];
        cb.fx9[1] = p.glow[1];
        cb.fx9[2] = p.glow[2] / float(std::max(1, tw));
        cb.fx9[3] = p.glow[2] / float(std::max(1, th));
    }
    if (p.cc.enabled) {
        cb.cc0[0] = p.cc.exposure;
        cb.cc0[1] = p.cc.contrast;
        cb.cc0[2] = p.cc.highlights;
        cb.cc0[3] = p.cc.shadows;
        cb.cc1[0] = p.cc.saturation;
        cb.cc1[1] = p.cc.temperature;
        cb.cc1[2] = p.cc.tint;
        cb.cc1[3] = 1;
    }
    if (p.lut && p.lutSize > 1) {
        srvs[4] = p.lut;
        cb.lutP[0] = p.lutIntensity;
        cb.lutP[1] = float(p.lutSize);
        cb.lutP[2] = 1;
        cb.lutP[3] = p.lutFirst ? 1.f : 0.f;
    }
    upload(ctx_, cbLayer_.Get(), cb);
    ID3D11RenderTargetView* rtv = canvas.rtv0.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(ctx_, 0, 0, float(canvas.width), float(canvas.height));
    setBlend(p.blend);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vsLayer_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cbLayer_.GetAddressOf());
    ctx_->PSSetShader(psLayer_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cbLayer_.GetAddressOf());
    ID3D11SamplerState* samps[2] = {linear_.Get(), point_.Get()};
    ctx_->PSSetSamplers(0, 2, samps);
    ctx_->PSSetShaderResources(0, 5, srvs);
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[5] = {};
    ctx_->PSSetShaderResources(0, 5, nulls);
}

void Compositor::drawTransition(const GpuTexture& dst, const GpuTexture& a, const GpuTexture& b, TransitionType type,
                                float progress) {
    TransitionCB cb{int(type), progress, {1.f / float(dst.width), 1.f / float(dst.height)}};
    upload(ctx_, cbTransition_.Get(), cb);
    ID3D11RenderTargetView* rtv = dst.rtv0.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(ctx_, 0, 0, float(dst.width), float(dst.height));
    setBlend(BlendMode::Normal);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsFull_.Get(), nullptr, 0);
    ctx_->PSSetShader(psTransition_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cbTransition_.GetAddressOf());
    ID3D11SamplerState* samps[2] = {linear_.Get(), point_.Get()};
    ctx_->PSSetSamplers(0, 2, samps);
    ID3D11ShaderResourceView* srvs[2] = {a.srv0.Get(), b.srv0.Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    ctx_->PSSetShaderResources(0, 2, nulls);
}

void Compositor::copy(const GpuTexture& dst, const GpuTexture& src) { ctx_->CopyResource(dst.tex.Get(), src.tex.Get()); }

void Compositor::composite(const GpuTexture& dst, const GpuTexture& src) {
    const float rect[4] = {0, 0, float(dst.width), float(dst.height)};
    const float bg[4] = {0, 0, 0, 0};
    blit(dst.rtv0.Get(), dst.width, dst.height, src, rect, bg, false, nullptr);
}

void Compositor::blit(ID3D11RenderTargetView* rtv, int rtW, int rtH, const GpuTexture& src, const float r[4],
                      const float bg[4], bool clearFirst, const float clearColor[4]) {
    if (clearFirst) ctx_->ClearRenderTargetView(rtv, clearColor);
    BlitCB cb{};
    cb.srcRect[0] = 0;
    cb.srcRect[1] = 0;
    cb.srcRect[2] = 1;
    cb.srcRect[3] = 1;
    std::memcpy(cb.color, bg, sizeof(cb.color));
    cb.params[3] = bg[3] >= 1.f ? 1.f : 0.f;
    upload(ctx_, cbBlit_.Get(), cb);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    float x0 = std::max(0.f, r[0]), y0 = std::max(0.f, r[1]);
    setViewport(ctx_, r[0], r[1], r[2] - r[0], r[3] - r[1]);
    (void)x0;
    (void)y0;
    (void)rtW;
    (void)rtH;
    ctx_->OMSetBlendState(bg[3] >= 1.f ? blendOff_.Get() : blendNormal_.Get(), nullptr, 0xffffffff);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsBlit_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cbBlit_.GetAddressOf());
    ctx_->PSSetShader(psBlit_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cbBlit_.GetAddressOf());
    ID3D11SamplerState* samps[2] = {linear_.Get(), point_.Get()};
    ctx_->PSSetSamplers(0, 2, samps);
    ID3D11ShaderResourceView* srv = src.srv0.Get();
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nullv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullv);
}

OverlayQuad OverlayQuad::rect(float x0, float y0, float x1, float y1, const float rgba[4]) {
    OverlayQuad q{{{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}}, {rgba[0], rgba[1], rgba[2], rgba[3]}};
    return q;
}

OverlayQuad OverlayQuad::line(float ax, float ay, float bx, float by, float th, const float rgba[4]) {
    float dx = bx - ax, dy = by - ay;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) len = 1e-3f;
    float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    OverlayQuad q{{{ax + nx, ay + ny}, {bx + nx, by + ny}, {ax - nx, ay - ny}, {bx - nx, by - ny}},
                  {rgba[0], rgba[1], rgba[2], rgba[3]}};
    return q;
}

void Compositor::drawQuads(ID3D11RenderTargetView* rtv, int rtW, int rtH, const std::vector<OverlayQuad>& quads) {
    if (quads.empty()) return;
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(ctx_, 0, 0, float(rtW), float(rtH));
    ctx_->OMSetBlendState(blendNormal_.Get(), nullptr, 0xffffffff);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vsRect_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {cbBlit_.Get(), cbQuad_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetShader(psRect_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cbBlit_.GetAddressOf());
    for (const auto& q : quads) {
        BlitCB cb{};
        for (int i = 0; i < 3; ++i) cb.color[i] = q.rgba[i] * q.rgba[3];
        cb.color[3] = q.rgba[3];
        upload(ctx_, cbBlit_.Get(), cb);
        float qc[4][4] = {};
        for (int i = 0; i < 4; ++i) {
            qc[i][0] = q.p[i][0] / float(rtW) * 2.f - 1.f;
            qc[i][1] = 1.f - q.p[i][1] / float(rtH) * 2.f;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(cbQuad_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, qc, sizeof(qc));
            ctx_->Unmap(cbQuad_.Get(), 0);
        }
        ctx_->Draw(4, 0);
    }
}
void Compositor::toNV12(const GpuTexture& canvas, const GpuTexture& nv12, ColorMatrix m) {
    BlitCB cb{};
    cb.srcRect[2] = 1;
    cb.srcRect[3] = 1;
    cb.params[0] = m == ColorMatrix::BT601 ? 1.f : 0.f;
    upload(ctx_, cbBlit_.Get(), cb);
    ctx_->OMSetBlendState(blendOff_.Get(), nullptr, 0xffffffff);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsBlit_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cbBlit_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, cbBlit_.GetAddressOf());
    ID3D11SamplerState* samps[2] = {linear_.Get(), point_.Get()};
    ctx_->PSSetSamplers(0, 2, samps);
    ID3D11ShaderResourceView* srv = canvas.srv0.Get();
    // luma
    ID3D11RenderTargetView* rtv = nv12.rtv0.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(ctx_, 0, 0, float(nv12.width), float(nv12.height));
    ctx_->PSSetShader(psLuma_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->Draw(3, 0);
    // chroma (half resolution)
    rtv = nv12.rtv1.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(ctx_, 0, 0, float(nv12.width / 2), float(nv12.height / 2));
    ctx_->PSSetShader(psChroma_.Get(), nullptr, 0);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nullv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullv);
    ID3D11RenderTargetView* nullr = nullptr;
    ctx_->OMSetRenderTargets(1, &nullr, nullptr);
}

ComPtr<ID3D11ShaderResourceView> Compositor::createLut(const Lut3D& lut) {
    if (!lut.valid()) return nullptr;
    const int n = lut.size;
    std::vector<float> rgba(size_t(n) * n * n * 4);
    for (size_t i = 0; i < size_t(n) * n * n; ++i) {
        rgba[i * 4] = lut.data[i * 3];
        rgba[i * 4 + 1] = lut.data[i * 3 + 1];
        rgba[i * 4 + 2] = lut.data[i * 3 + 2];
        rgba[i * 4 + 3] = 1;
    }
    D3D11_TEXTURE3D_DESC d{};
    d.Width = d.Height = d.Depth = UINT(n);
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{rgba.data(), UINT(n * 16), UINT(n * n * 16)};
    ComPtr<ID3D11Texture3D> t;
    if (FAILED(gpu_->device()->CreateTexture3D(&d, &sd, &t))) return nullptr;
    ComPtr<ID3D11ShaderResourceView> srv;
    gpu_->device()->CreateShaderResourceView(t.Get(), nullptr, &srv);
    return srv;
}

GpuTexturePtr Compositor::uploadBGRA(int w, int h, const void* data, int pitch) {
    GpuTexturePtr t = gpu_->acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    if (t) ctx_->UpdateSubresource(t->tex.Get(), 0, nullptr, data, UINT(pitch), 0);
    return t;
}

bool Compositor::readback(const GpuTexture& t, std::vector<uint8_t>& out, int& rowPitch) {
    if (stagingW_ != t.width || stagingH_ != t.height || stagingFmt_ != t.format) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(t.width);
        d.Height = UINT(t.height);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = t.format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingRb_.Reset();
        if (FAILED(gpu_->device()->CreateTexture2D(&d, nullptr, &stagingRb_))) return false;
        stagingW_ = t.width;
        stagingH_ = t.height;
        stagingFmt_ = t.format;
    }
    ctx_->CopyResource(stagingRb_.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(stagingRb_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    size_t rows = size_t(t.height);
    if (t.format == DXGI_FORMAT_NV12) rows = rows * 3 / 2;
    if (t.format == DXGI_FORMAT_P010) rows = rows * 3 / 2;
    rowPitch = int(m.RowPitch);
    out.resize(size_t(m.RowPitch) * rows);
    std::memcpy(out.data(), m.pData, out.size());
    ctx_->Unmap(stagingRb_.Get(), 0);
    return true;
}

void Compositor::flush() { ctx_->Flush(); }

} // namespace cut
