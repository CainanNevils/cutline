#pragma once
// Shared D3D11 device, pooled textures and GPU-resident video frames.
//
// Threading: one ID3D11Device per GpuDevice with multithread protection ON. The immediate
// context is driven by exactly one render owner (preview render thread, or an export/proxy job
// with its own GpuDevice). Decoder threads only issue CopySubresourceRegion/UpdateSubresource
// into textures they own (and FFmpeg's own video-decode calls), which the D3D11 runtime
// serializes per call; they never touch pipeline state.
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/model.h"

namespace cut {

using Microsoft::WRL::ComPtr;

enum class PixelLayout : uint8_t {
    NV12 = 0,   // 8-bit 4:2:0, two planes
    P010 = 1,   // 10-bit 4:2:0 in 16-bit containers
    BGRA = 2,   // straight alpha (decoded stills, SW fallback)
    BGRAPremul = 3,  // premultiplied (text)
    RGBA16F = 4,
};

struct ColorInfo {
    ColorRange range = ColorRange::Limited;
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorTransfer transfer = ColorTransfer::SDR;
};

class GpuDevice;

// A pooled 2D texture; returned to its pool when the last reference drops.
struct GpuTexture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv0, srv1;  // plane views (Y/UV) or the single RGBA view
    ComPtr<ID3D11RenderTargetView> rtv0, rtv1;
    int width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0;
    size_t bytes = 0;
};
using GpuTexturePtr = std::shared_ptr<GpuTexture>;

struct GpuFrame {
    GpuTexturePtr tex;
    PixelLayout layout = PixelLayout::NV12;
    int width = 0, height = 0;  // visible size
    ColorInfo color;
    Tick pts = 0;                // source time (ticks, media start = 0)
    std::atomic<Tick> end{0};    // pts of the next frame (or estimate)
    bool hw = false;             // decoded by the GPU
    size_t bytes() const { return tex ? tex->bytes : 0; }
};
using GpuFramePtr = std::shared_ptr<GpuFrame>;

class GpuDevice : public std::enable_shared_from_this<GpuDevice> {
public:
    static std::shared_ptr<GpuDevice> create(bool debugLayer, std::string& error, int adapterIndex = -1);
    ~GpuDevice();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11Device1* device1() const { return device1_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }
    IDXGIFactory2* factory() const { return factory_.Get(); }
    const std::string& adapterName() const { return adapterName_; }
    uint32_t vendorId() const { return vendor_; }
    size_t dedicatedVram() const { return vram_; }
    bool isNvidia() const { return vendor_ == 0x10DE; }
    bool removed() const;
    std::string removedReason() const;

    // Pooled textures. bind: D3D11_BIND_* flags. Thread-safe.
    GpuTexturePtr acquire(int w, int h, DXGI_FORMAT fmt, UINT bind);
    GpuTexturePtr createUnpooled(int w, int h, DXGI_FORMAT fmt, UINT bind, const void* data = nullptr,
                                 UINT pitch = 0);
    size_t textureBytesInUse() const { return inUse_.load(); }
    size_t textureBytesPooled() const { return pooled_.load(); }
    void trimPool(size_t keepBytes = 0);

    // Process-wide VRAM usage from DXGI (local segment).
    size_t vramUsage() const;

    // Opaque per-device FFmpeg hardware device (AVBufferRef*), created on demand by media/.
    void* ffHwDevice = nullptr;
    std::function<void(void*)> ffHwDeviceFree;
    std::mutex ffMutex;

private:
    GpuDevice() = default;
    void release(GpuTexture* t);
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Device1> device1_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGIAdapter3> adapter_;
    std::string adapterName_;
    uint32_t vendor_ = 0;
    size_t vram_ = 0;

    struct PoolKey {
        int w, h;
        DXGI_FORMAT f;
        UINT bind;
        bool operator==(const PoolKey& o) const { return w == o.w && h == o.h && f == o.f && bind == o.bind; }
    };
    struct PoolHash {
        size_t operator()(const PoolKey& k) const {
            return (size_t(k.w) * 73856093u) ^ (size_t(k.h) * 19349663u) ^ (size_t(k.f) * 83492791u) ^ k.bind;
        }
    };
    mutable std::mutex poolMutex_;
    std::unordered_map<PoolKey, std::vector<std::unique_ptr<GpuTexture>>, PoolHash> pool_;
    std::atomic<size_t> inUse_{0}, pooled_{0};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

size_t textureBytes(int w, int h, DXGI_FORMAT f);
void createViews(ID3D11Device* dev, GpuTexture& t);

// Throws nothing; logs HRESULT failures in debug builds.
std::string hrString(HRESULT hr);

} // namespace cut
