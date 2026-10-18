#include "render/swapchain.h"

namespace cut {

bool SwapChain::create(GpuDevice& gpu, HWND hwnd, int w, int h, std::string& error) {
    destroy();
    gpu_ = &gpu;
    hwnd_ = hwnd;
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = UINT(w_);
    d.Height = UINT(h_);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = gpu.factory()->CreateSwapChainForHwnd(gpu.device(), hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        error = "CreateSwapChainForHwnd failed " + hrString(hr);
        return false;
    }
    gpu.factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    sc1.As(&sc_);
    if (!sc_) {
        error = "IDXGISwapChain2 unavailable";
        return false;
    }
    sc_->SetMaximumFrameLatency(1);
    waitable_ = sc_->GetFrameLatencyWaitableObject();
    makeRtv();
    return true;
}

void SwapChain::destroy() {
    rtv_.Reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    sc_.Reset();
}

void SwapChain::makeRtv() {
    rtv_.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (SUCCEEDED(sc_->GetBuffer(0, IID_PPV_ARGS(&bb)))) gpu_->device()->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
}

bool SwapChain::resize(int w, int h) {
    if (!sc_) return false;
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == w_ && h == h_) return true;
    rtv_.Reset();
    gpu_->context()->OMSetRenderTargets(0, nullptr, nullptr);
    gpu_->context()->Flush();
    HRESULT hr = sc_->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) return false;
    w_ = w;
    h_ = h;
    makeRtv();
    return true;
}

HRESULT SwapChain::present(bool vsync) {
    if (!sc_) return E_FAIL;
    return sc_->Present(vsync ? 1 : 0, 0);
}

} // namespace cut
