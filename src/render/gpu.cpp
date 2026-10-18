#include "render/gpu.h"

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace cut {

std::string hrString(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

size_t textureBytes(int w, int h, DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_NV12: return size_t(w) * h * 3 / 2;
    case DXGI_FORMAT_P010: return size_t(w) * h * 3;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return size_t(w) * h * 8;
    case DXGI_FORMAT_R8_UNORM: return size_t(w) * h;
    case DXGI_FORMAT_R8G8_UNORM: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
    }
}

void createViews(ID3D11Device* dev, GpuTexture& t) {
    const bool planar = t.format == DXGI_FORMAT_NV12 || t.format == DXGI_FORMAT_P010;
    if (t.bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (planar) {
            sd.Format = t.format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
            dev->CreateShaderResourceView(t.tex.Get(), &sd, &t.srv0);
            sd.Format = t.format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
            dev->CreateShaderResourceView(t.tex.Get(), &sd, &t.srv1);
        } else {
            sd.Format = t.format;
            dev->CreateShaderResourceView(t.tex.Get(), &sd, &t.srv0);
        }
    }
    if (t.bind & D3D11_BIND_RENDER_TARGET) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (planar) {
            rd.Format = t.format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
            dev->CreateRenderTargetView(t.tex.Get(), &rd, &t.rtv0);
            rd.Format = t.format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
            dev->CreateRenderTargetView(t.tex.Get(), &rd, &t.rtv1);
        } else {
            rd.Format = t.format;
            dev->CreateRenderTargetView(t.tex.Get(), &rd, &t.rtv0);
        }
    }
}

std::shared_ptr<GpuDevice> GpuDevice::create(bool debugLayer, std::string& error, int adapterIndex) {
    std::shared_ptr<GpuDevice> g(new GpuDevice());
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = "CreateDXGIFactory2 failed " + hrString(hr);
        return nullptr;
    }
    // Pick the adapter: explicit index, else the one with the most dedicated VRAM (the dGPU).
    ComPtr<IDXGIAdapter1> best;
    size_t bestVram = 0;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            a.Reset();
            continue;
        }
        if (adapterIndex >= 0 ? int(i) == adapterIndex : d.DedicatedVideoMemory > bestVram) {
            best = a;
            bestVram = d.DedicatedVideoMemory;
        }
        a.Reset();
    }
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    hr = D3D11CreateDevice(best.Get(), best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                           levels, 2, D3D11_SDK_VERSION, &g->device_, &got, &g->ctx_);
    if (FAILED(hr) && debugLayer) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;  // SDK layers not installed
        hr = D3D11CreateDevice(best.Get(), best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, 2, D3D11_SDK_VERSION, &g->device_, &got, &g->ctx_);
    }
    if (FAILED(hr)) {
        error = "D3D11CreateDevice failed " + hrString(hr) + " (a DirectX 11 GPU is required)";
        return nullptr;
    }
    g->device_.As(&g->device1_);
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(g->ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dxgiDev;
    g->device_.As(&dxgiDev);
    ComPtr<IDXGIAdapter> ad;
    dxgiDev->GetAdapter(&ad);
    ad.As(&g->adapter_);
    ComPtr<IDXGIFactory2> f2;
    ad->GetParent(IID_PPV_ARGS(&f2));
    g->factory_ = f2 ? f2 : factory;
    DXGI_ADAPTER_DESC ddesc;
    ad->GetDesc(&ddesc);
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, ddesc.Description, -1, name, sizeof(name), nullptr, nullptr);
    g->adapterName_ = name;
    g->vendor_ = ddesc.VendorId;
    g->vram_ = ddesc.DedicatedVideoMemory;
    if (debugLayer) {
        ComPtr<ID3D11InfoQueue> q;
        if (SUCCEEDED(g->device_.As(&q))) {
            q->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            q->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }
    return g;
}

GpuDevice::~GpuDevice() {
    *alive_ = false;
    {
        std::lock_guard<std::mutex> l(poolMutex_);
        pool_.clear();
    }
    if (ffHwDevice && ffHwDeviceFree) ffHwDeviceFree(ffHwDevice);
    if (ctx_) {
        ctx_->ClearState();
        ctx_->Flush();
    }
}

bool GpuDevice::removed() const { return device_ && device_->GetDeviceRemovedReason() != S_OK; }

std::string GpuDevice::removedReason() const {
    return device_ ? hrString(device_->GetDeviceRemovedReason()) : "no device";
}

size_t GpuDevice::vramUsage() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (adapter_ && SUCCEEDED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return size_t(info.CurrentUsage);
    return 0;
}

GpuTexturePtr GpuDevice::acquire(int w, int h, DXGI_FORMAT fmt, UINT bind) {
    std::unique_ptr<GpuTexture> t;
    PoolKey key{w, h, fmt, bind};
    {
        std::lock_guard<std::mutex> l(poolMutex_);
        auto it = pool_.find(key);
        if (it != pool_.end() && !it->second.empty()) {
            t = std::move(it->second.back());
            it->second.pop_back();
            pooled_ -= t->bytes;
        }
    }
    if (!t) {
        t = std::make_unique<GpuTexture>();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(w);
        d.Height = UINT(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &t->tex))) return nullptr;
        t->width = w;
        t->height = h;
        t->format = fmt;
        t->bind = bind;
        t->bytes = textureBytes(w, h, fmt);
        createViews(device_.Get(), *t);
    }
    inUse_ += t->bytes;
    std::weak_ptr<bool> alive = alive_;
    GpuDevice* self = this;
    GpuTexture* raw = t.release();
    return GpuTexturePtr(raw, [self, alive](GpuTexture* p) {
        auto a = alive.lock();
        if (a && *a) self->release(p);
        else delete p;
    });
}

void GpuDevice::release(GpuTexture* t) {
    inUse_ -= t->bytes;
    std::lock_guard<std::mutex> l(poolMutex_);
    // Keep at most ~512 MB of free textures pooled.
    if (pooled_ + t->bytes > (size_t(512) << 20)) {
        delete t;
        return;
    }
    pooled_ += t->bytes;
    pool_[{t->width, t->height, t->format, t->bind}].emplace_back(t);
}

void GpuDevice::trimPool(size_t keep) {
    std::lock_guard<std::mutex> l(poolMutex_);
    for (auto& [k, v] : pool_) {
        while (!v.empty() && pooled_ > keep) {
            pooled_ -= v.back()->bytes;
            v.pop_back();
        }
    }
}

GpuTexturePtr GpuDevice::createUnpooled(int w, int h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT pitch) {
    auto t = std::make_shared<GpuTexture>();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
    if (FAILED(device_->CreateTexture2D(&d, data ? &sd : nullptr, &t->tex))) return nullptr;
    t->width = w;
    t->height = h;
    t->format = fmt;
    t->bind = bind;
    t->bytes = textureBytes(w, h, fmt);
    createViews(device_.Get(), *t);
    return t;
}

} // namespace cut
