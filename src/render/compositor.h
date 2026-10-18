#pragma once
// GPU compositor. Owned by exactly one thread (the owner of the device's immediate context).
#include "core/lut.h"
#include "core/model.h"
#include "render/gpu.h"

#include <array>
#include <vector>

namespace cut {

struct Corners {
    float p[4][2];  // sequence pixel coords: TL, TR, BL, BR
};

struct ColorCorrection {
    bool enabled = false;
    float exposure = 0, contrast = 0, highlights = 0, shadows = 0;  // contrast/hl/sh in -1..1
    float saturation = 1, temperature = 0, tint = 0;
};

struct LayerParams {
    const GpuFrame* frame = nullptr;    // decoded source, or
    const GpuTexture* rgba = nullptr;   // RGBA texture (text): premultiplied if rgbaPremul
    bool rgbaPremul = true;
    const GpuFrame* frame2 = nullptr;   // frame blending partner
    float blend2 = 0;
    Corners corners{};
    float crop[4] = {0, 0, 0, 0};  // fraction removed: left, top, right, bottom
    float opacity = 1;
    BlendMode blend = BlendMode::Normal;
    ColorCorrection cc;
    ID3D11ShaderResourceView* lut = nullptr;
    int lutSize = 0;
    float lutIntensity = 1;
    bool lutFirst = false;  // LUT before colour correction (effect stack order)
    // stylize
    bool flipH = false, flipV = false;
    float blurPx = 0;        // gaussian blur radius in source pixels
    float sharpen = 0;       // unsharp amount (0..2)
    float blackWhite = 0;    // 0..1
    float vignette[3] = {0, 0.5f, 0.5f};  // amount (-1..1, negative lightens), midpoint, feather
    float cornerRadius = 0, borderWidth = 0;  // source pixels
    float borderColor[3] = {1, 1, 1};
    float shadowOpacity = 0, shadowDx = 0, shadowDy = 0, shadowSoft = 0;  // drop shadow (source px)
    // pixel size the effects measure in when the texture is not 1:1 with the source (mattes,
    // adjustment layers and nests rendered at preview scale); 0 = the texture size
    int logicalW = 0, logicalH = 0;
    float glow[3] = {0, 0.7f, 24};  // intensity (0 = off), threshold, radius (source px)
    int keyColor = 0;  // chroma key: 0 off, 1 green, 2 blue
    float keyTolerance = 0.35f, keySoftness = 0.1f, keySpill = 0.5f;
    int maskShape = 0;  // 0 off, 1 ellipse, 2 rectangle
    float mask[4] = {0.5f, 0.5f, 0.5f, 0.5f};  // centre x, y, width, height (fractions of the image)
    float maskFeather = 20;                    // source px
    bool maskInvert = false;
    float mosaic = 0;  // block size in source px (0 = off)
};

// Solid quad in render-target pixels (TL, TR, BL, BR). Rects and thick lines build these.
struct OverlayQuad {
    float p[4][2];
    float rgba[4];
    static OverlayQuad rect(float x0, float y0, float x1, float y1, const float rgba[4]);
    static OverlayQuad line(float ax, float ay, float bx, float by, float thickness, const float rgba[4]);
};

// Affine placement of a source in the sequence frame (Motion effect + fit mode).
struct MotionValues {
    double posX = 0, posY = 0;          // offset from frame centre (px)
    double scale = 100, scaleW = 100;   // percent
    bool uniform = true;
    double rotation = 0;                // degrees, clockwise
    double anchorX = 0, anchorY = 0;    // offset from source centre (px)
};
double fitScale(FitMode fit, int srcW, int srcH, int seqW, int seqH);
Corners computeCorners(int srcW, int srcH, int seqW, int seqH, FitMode fit, const MotionValues& m);

class Compositor {
public:
    bool init(std::shared_ptr<GpuDevice> gpu, std::string& error);
    GpuDevice& gpu() { return *gpu_; }
    std::shared_ptr<GpuDevice> gpuPtr() { return gpu_; }

    GpuTexturePtr makeCanvas(int w, int h, bool hq);
    void clear(const GpuTexture& t, float r = 0, float g = 0, float b = 0, float a = 0);
    void drawLayer(const GpuTexture& canvas, int seqW, int seqH, const LayerParams& p);
    // dst = transition(a, b) composited over dst (normal blend)
    void drawTransition(const GpuTexture& dst, const GpuTexture& a, const GpuTexture& b, TransitionType type,
                        float progress);
    // src over dst
    void composite(const GpuTexture& dst, const GpuTexture& src);
    // exact copy (same size and format)
    void copy(const GpuTexture& dst, const GpuTexture& src);
    // canvas over `bg` into an arbitrary RTV (swapchain / thumbnail) at destRect (pixels)
    void blit(ID3D11RenderTargetView* rtv, int rtW, int rtH, const GpuTexture& src, const float destRect[4],
              const float bg[4], bool clearFirst, const float clearColor[4]);
    void drawQuads(ID3D11RenderTargetView* rtv, int rtW, int rtH, const std::vector<OverlayQuad>& quads);
    // canvas -> NV12 texture (must have RTVs on both planes)
    void toNV12(const GpuTexture& canvas, const GpuTexture& nv12, ColorMatrix m);

    ComPtr<ID3D11ShaderResourceView> createLut(const Lut3D& lut);
    GpuTexturePtr uploadBGRA(int w, int h, const void* data, int pitch);
    // GPU -> CPU (BGRA8 canvas or NV12); waits for the GPU.
    bool readback(const GpuTexture& t, std::vector<uint8_t>& out, int& rowPitch);
    void flush();

private:
    void setBlend(BlendMode m);
    std::shared_ptr<GpuDevice> gpu_;
    ID3D11DeviceContext* ctx_ = nullptr;
    ComPtr<ID3D11VertexShader> vsLayer_, vsFull_, vsBlit_, vsRect_;
    ComPtr<ID3D11PixelShader> psLayer_, psTransition_, psBlit_, psRect_, psLuma_, psChroma_;
    ComPtr<ID3D11Buffer> cbLayer_, cbTransition_, cbBlit_, cbQuad_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11BlendState> blendNormal_, blendAdd_, blendMultiply_, blendScreen_, blendOff_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Texture2D> stagingRb_;
    int stagingW_ = 0, stagingH_ = 0;
    DXGI_FORMAT stagingFmt_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace cut
