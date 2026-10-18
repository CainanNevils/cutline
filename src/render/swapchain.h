#pragma once
#include "render/gpu.h"

namespace cut {

// DXGI flip-model swapchain on a native child window. Created on the render thread from the
// shared device; Qt never draws into the window.
class SwapChain {
public:
    bool create(GpuDevice& gpu, HWND hwnd, int w, int h, std::string& error);
    void destroy();
    bool valid() const { return sc_ != nullptr; }
    bool resize(int w, int h);
    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    int width() const { return w_; }
    int height() const { return h_; }
    HWND hwnd() const { return hwnd_; }
    // Returns the Present HRESULT (DXGI_ERROR_DEVICE_REMOVED etc. propagate).
    HRESULT present(bool vsync);
    HANDLE waitable() const { return waitable_; }

private:
    void makeRtv();
    GpuDevice* gpu_ = nullptr;
    ComPtr<IDXGISwapChain2> sc_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    HWND hwnd_ = nullptr;
    int w_ = 0, h_ = 0;
};

} // namespace cut
