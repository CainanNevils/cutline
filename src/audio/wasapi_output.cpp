#include "audio/audio_output.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <vector>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace cut {

using Microsoft::WRL::ComPtr;

namespace {

class WasapiOutput final : public AudioOutput {
public:
    explicit WasapiOutput(std::string id) : deviceId_(std::move(id)) {}
    ~WasapiOutput() override { stop(); }

    bool start(PullFn pull, std::string& error) override {
        stop();
        pull_ = std::move(pull);
        std::atomic<int> ready{0};
        std::string err;
        thread_ = std::thread([this, &ready, &err] { run(ready, err); });
        while (ready.load() == 0) std::this_thread::yield();
        if (ready.load() < 0) {
            thread_.join();
            error = err;
            return false;
        }
        return true;
    }

    void stop() override {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        quit_ = false;
        running_ = false;
    }

    bool running() const override { return running_; }

    int64_t playedFrames() const override {
        // Interpolate the last device position with QPC.
        int64_t pos = lastPos_.load();
        int64_t qpc = lastQpc_.load();
        if (!running_ || qpc == 0) return pos;
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        int64_t dt = (now.QuadPart - qpc) * 48000 / f.QuadPart;
        int64_t p = pos + std::min<int64_t>(dt, 48000 / 10);  // never extrapolate more than 100 ms
        int64_t prev = lastReported_.load();
        if (p < prev) p = prev;
        lastReported_.store(p);
        return p;
    }

    double latencyMs() const override { return latencyMs_; }
    double bufferMs() const override { return bufferMs_; }
    int64_t prefillFrames() const override { return prefill_; }
    std::string name() const override {
        return "WASAPI: " + deviceName_ + (lowLatency_ ? " (low-latency, " + std::to_string(periodFrames_ * 1000 / 48000.0).substr(0, 4) + " ms periods)" : "");
    }
    uint64_t glitches() const override { return glitches_; }

private:
    void run(std::atomic<int>& ready, std::string& err) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool coInit = SUCCEEDED(hr);
        auto fail = [&](const char* what, HRESULT h) {
            char buf[64];
            snprintf(buf, sizeof(buf), " (0x%08lX)", (unsigned long)h);
            err = std::string("WASAPI: ") + what + buf;
            ready = -1;
            if (coInit) CoUninitialize();
        };
        ComPtr<IMMDeviceEnumerator> en;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr)) return fail("no device enumerator", hr);
        ComPtr<IMMDevice> dev;
        if (!deviceId_.empty()) {
            wchar_t wid[512];
            MultiByteToWideChar(CP_UTF8, 0, deviceId_.c_str(), -1, wid, 512);
            hr = en->GetDevice(wid, &dev);
        } else hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) return fail("no default output device", hr);
        {
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                    char name[256];
                    WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name), nullptr, nullptr);
                    deviceName_ = name;
                }
                PropVariantClear(&v);
            }
        }
        ComPtr<IAudioClient> client;
        // Low-latency shared mode (IAudioClient3, Windows 10+): small engine periods when the
        // device mix format already is 48 kHz float stereo (no conversion allowed on this path).
        lowLatency_ = false;
        if (std::getenv("CUTLINE_AUDIO_LEGACY") == nullptr) {
            ComPtr<IAudioClient3> c3;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3))) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(c3->GetMixFormat(&mix)) && mix) {
                    const auto* mx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
                    const bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                         (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                          mx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                    UINT32 def = 0, fund = 0, mn = 0, mxp = 0;
                    if (isFloat && mix->nChannels == 2 && mix->nSamplesPerSec == 48000 && mix->wBitsPerSample == 32 &&
                        SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mxp)) && fund > 0 && mn < def) {
                        // ~5 ms periods: low enough for the < 20 ms target, roomy enough for the copy thread
                        UINT32 period = std::max(mn, ((240 + fund - 1) / fund) * fund);
                        period = std::min(period, mxp);
                        if (SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, mix, nullptr))) {
                            client = c3;
                            lowLatency_ = true;
                            periodFrames_ = period;
                        }
                    }
                    CoTaskMemFree(mix);
                }
            }
        }
        if (!client) {
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            if (FAILED(hr)) return fail("activate failed", hr);
        }
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = 48000;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = 48000 * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        // 10 ms requested buffer; the engine converts to the device mix format if it differs.
        const REFERENCE_TIME bufferHns = 100000;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (!lowLatency_) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferHns, 0, &wf.Format, nullptr);
            if (FAILED(hr)) return fail("initialize failed", hr);
        }
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client->SetEventHandle(ev);
        UINT32 bufFrames = 0;
        client->GetBufferSize(&bufFrames);
        REFERENCE_TIME lat = 0;
        client->GetStreamLatency(&lat);
        bufferMs_ = bufFrames * 1000.0 / 48000.0;
        latencyMs_ = double(lat) / 10000.0 + bufferMs_;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioClock> clock;
        client->GetService(IID_PPV_ARGS(&render));
        client->GetService(IID_PPV_ARGS(&clock));
        UINT64 freq = 48000;
        if (clock) clock->GetFrequency(&freq);
        // prefill silence
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufFrames, &data))) render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        prefill_ = bufFrames;
        DWORD taskIdx = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
        hr = client->Start();
        if (FAILED(hr)) {
            CloseHandle(ev);
            return fail("start failed", hr);
        }
        running_ = true;
        lastPos_ = 0;
        lastQpc_ = 0;
        lastReported_ = 0;
        ready = 1;
        while (!quit_) {
            DWORD w = WaitForSingleObject(ev, 200);
            if (w != WAIT_OBJECT_0) {
                ++glitches_;  // device stalled
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;  // device lost
            UINT32 avail = bufFrames - padding;
            if (avail > 0 && SUCCEEDED(render->GetBuffer(avail, &data))) {
                pull_(reinterpret_cast<float*>(data), int(avail));
                render->ReleaseBuffer(avail, 0);
            }
            if (clock) {
                UINT64 pos = 0, qpcPos = 0;
                if (SUCCEEDED(clock->GetPosition(&pos, &qpcPos))) {
                    // qpcPos is in 100 ns units
                    LARGE_INTEGER f;
                    QueryPerformanceFrequency(&f);
                    lastPos_ = int64_t(pos * 48000 / std::max<UINT64>(1, freq));
                    lastQpc_ = int64_t(qpcPos) * f.QuadPart / 10000000;
                }
            }
        }
        client->Stop();
        running_ = false;
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CloseHandle(ev);
        if (coInit) CoUninitialize();
    }

    PullFn pull_;
    std::thread thread_;
    std::atomic<bool> quit_{false}, running_{false};
    std::atomic<int64_t> lastPos_{0}, lastQpc_{0};
    mutable std::atomic<int64_t> lastReported_{0};
    std::atomic<uint64_t> glitches_{0};
    double latencyMs_ = 0, bufferMs_ = 0;
    bool lowLatency_ = false;
    UINT32 periodFrames_ = 0;
    std::atomic<int64_t> prefill_{0};
    std::string deviceName_ = "default";
    std::string deviceId_;
};

} // namespace

std::unique_ptr<AudioOutput> createWasapiOutput(const std::string& id) { return std::make_unique<WasapiOutput>(id); }

std::unique_ptr<AudioOutput> createAudioOutput(AudioOutput::PullFn pull, std::string& backend, std::string& error,
                                               const std::string& deviceIdIn) {
    std::string deviceId = deviceIdIn;
    if (const char* e = std::getenv("CUTLINE_AUDIO_DEVICE"); e && *e) deviceId = e;  // tests: always wins
    auto w = createWasapiOutput(deviceId);
    std::string e1;
    if (w->start(pull, e1)) {
        backend = "WASAPI";
        return w;
    }
    if (!deviceId.empty()) {
        if (std::getenv("CUTLINE_AUDIO_DEVICE")) {
            error = e1;  // tests: never fall back to another (audible) device
            return nullptr;
        }
        // a chosen device that is gone (unplugged): use the default one
        w = createWasapiOutput({});
        std::string e3;
        if (w->start(pull, e3)) {
            backend = "WASAPI";
            return w;
        }
    }
    auto q = createQtAudioOutput();
    std::string e2;
    if (q && q->start(pull, e2)) {
        backend = "Qt";
        return q;
    }
    error = e1 + (e2.empty() ? "" : "; " + e2);
    return nullptr;
}

} // namespace cut

namespace cut {
std::vector<std::pair<std::string, std::string>> listAudioDevices() {
    std::vector<std::pair<std::string, std::string>> out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co = SUCCEEDED(hr);
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDeviceCollection> col;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                ComPtr<IMMDevice> d;
                if (FAILED(col->Item(i, &d))) continue;
                LPWSTR id = nullptr;
                d->GetId(&id);
                char idu[512] = {}, name[256] = {};
                if (id) WideCharToMultiByte(CP_UTF8, 0, id, -1, idu, sizeof(idu), nullptr, nullptr);
                CoTaskMemFree(id);
                ComPtr<IPropertyStore> props;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name), nullptr, nullptr);
                    PropVariantClear(&v);
                }
                out.push_back({idu, name});
            }
        }
    }
    if (co) CoUninitialize();
    return out;
}

std::string defaultAudioDeviceId() {
    std::string out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co = SUCCEEDED(hr);
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> d;
        LPWSTR id = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)) && SUCCEEDED(d->GetId(&id))) {
            char idu[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, id, -1, idu, sizeof(idu), nullptr, nullptr);
            out = idu;
        }
        CoTaskMemFree(id);
    }
    if (co) CoUninitialize();
    return out;
}

} // namespace cut