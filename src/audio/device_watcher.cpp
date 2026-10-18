#include "audio/device_watcher.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>

namespace cut {

using Microsoft::WRL::ComPtr;

namespace {

class Client : public IMMNotificationClient {
public:
    explicit Client(std::function<void()> fn) : fn_(std::move(fn)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    void disable() { enabled_ = false; }

private:
    void notify() {
        if (enabled_ && fn_) fn_();
    }
    std::function<void()> fn_;
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> enabled_{true};
};

} // namespace

struct AudioDeviceWatcher::Impl {
    ComPtr<IMMDeviceEnumerator> en;
    Client* client = nullptr;
};

AudioDeviceWatcher::AudioDeviceWatcher(std::function<void()> onChange) : d_(std::make_unique<Impl>()) {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d_->en)))) return;
    d_->client = new Client(std::move(onChange));
    if (FAILED(d_->en->RegisterEndpointNotificationCallback(d_->client))) {
        d_->client->Release();
        d_->client = nullptr;
    }
}

AudioDeviceWatcher::~AudioDeviceWatcher() {
    if (d_->client) {
        d_->client->disable();
        d_->en->UnregisterEndpointNotificationCallback(d_->client);
        d_->client->Release();
    }
}

bool AudioDeviceWatcher::active() const { return d_->client != nullptr; }

} // namespace cut
