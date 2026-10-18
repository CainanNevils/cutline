#include "audio/recorder.h"

#include "media/ff.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace cut {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

std::string narrow(const wchar_t* w) {
    char buf[512] = {};
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

// WAV (IEEE float, 48 kHz stereo) written incrementally; sizes filled in at the end.
struct WavWriter {
    FILE* f = nullptr;
    uint32_t dataBytes = 0;
    bool open(const std::string& path) {
        f = _wfopen(widen(path).c_str(), L"wb");
        if (!f) return false;
        writeHeader();
        return true;
    }
    void writeHeader() {
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(3);  // IEEE float
        u16(2);
        u32(48000);
        u32(48000 * 8);
        u16(8);
        u16(32);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
    }
    void write(const float* p, int frames) {
        if (!f || frames <= 0) return;
        std::fwrite(p, 8, size_t(frames), f);
        dataBytes += uint32_t(frames) * 8;
    }
    void close() {
        if (!f) return;
        std::fseek(f, 0, SEEK_SET);
        writeHeader();
        std::fclose(f);
        f = nullptr;
    }
};

} // namespace

AudioRecorder::AudioRecorder() = default;
AudioRecorder::~AudioRecorder() { stop(); }

float AudioRecorder::peak() { return peak_.exchange(0.f); }

bool AudioRecorder::start(const std::string& wavPath, const std::string& deviceId, bool loopback, std::string& error) {
    stop();
    path_ = wavPath;
    quit_ = false;
    ready_ = false;
    failed_ = false;
    frames_ = 0;
    firstUs_ = 0;
    thread_ = std::thread([this, deviceId, loopback] { run(deviceId, loopback); });
    while (!ready_.load() && !failed_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (failed_.load()) {
        thread_.join();
        error = error_;
        return false;
    }
    return true;
}

void AudioRecorder::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void AudioRecorder::run(std::string deviceId, bool loopback) {
    const bool coInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    auto fail = [&](const std::string& e) {
        error_ = e;
        failed_ = true;
        if (coInit) CoUninitialize();
    };
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return fail("No audio devices.");
    ComPtr<IMMDevice> dev;
    HRESULT hr = deviceId.empty() ? en->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &dev)
                                  : en->GetDevice(widen(deviceId).c_str(), &dev);
    if (FAILED(hr)) return fail(loopback ? "Output device not found." : "No microphone found.");
    {
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) deviceName_ = narrow(v.pwszVal);
            PropVariantClear(&v);
        }
    }
    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return fail("Cannot open the device.");
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return fail("Cannot read the device format.");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mixHold(mix, &CoTaskMemFree);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 1000000, 0, mix, nullptr);
    if (FAILED(hr)) return fail("Cannot start recording (device busy?).");
    ComPtr<IAudioCaptureClient> cap;
    if (FAILED(client->GetService(IID_PPV_ARGS(&cap)))) return fail("Cannot start recording.");

    // device format -> 48 kHz float stereo
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
    const bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    AVSampleFormat inFmt = isFloat ? AV_SAMPLE_FMT_FLT : mix->wBitsPerSample == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout, mix->nChannels);
    av_channel_layout_default(&outLayout, 2);
    SwrContext* swrRaw = nullptr;
    if (swr_alloc_set_opts2(&swrRaw, &outLayout, AV_SAMPLE_FMT_FLT, 48000, &inLayout, inFmt, int(mix->nSamplesPerSec), 0, nullptr) < 0) {
        swr_free(&swrRaw);
        return fail("Unsupported device format.");
    }
    // a mono microphone goes to both channels at full level (swr's default puts centre at -3 dB)
    av_opt_set_double(swrRaw, "center_mix_level", 1.0, 0);
    if (swr_init(swrRaw) < 0) {
        swr_free(&swrRaw);
        return fail("Unsupported device format.");
    }
    ff::SwrPtr swr(swrRaw);
    WavWriter wav;
    if (!wav.open(path_)) return fail("Cannot write " + path_);
    if (FAILED(client->Start())) {
        wav.close();
        return fail("Cannot start recording.");
    }
    running_ = true;
    ready_ = true;
    std::vector<float> out;
    std::vector<uint8_t> silence;
    while (!quit_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            if (FAILED(cap->GetBuffer(&data, &n, &flags, &devPos, &qpcPos))) break;
            // qpcPos is QPC in 100 ns units; MSVC's steady_clock is QPC too, so this matches engine nowUs()
            if (firstUs_.load() == 0 && n > 0) firstUs_ = int64_t(qpcPos / 10);
            const int maxOut = swr_get_out_samples(swr.get(), int(n)) + 64;
            out.resize(size_t(maxOut) * 2);
            const uint8_t* in = data;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign(size_t(n) * mix->nBlockAlign, 0);
                in = silence.data();
            }
            uint8_t* o = reinterpret_cast<uint8_t*>(out.data());
            const int got = swr_convert(swr.get(), &o, maxOut, &in, int(n));
            cap->ReleaseBuffer(n);
            if (got > 0) {
                float pk = peak_.load();
                for (int i = 0; i < got * 2; ++i) pk = std::max(pk, std::abs(out[size_t(i)]));
                peak_ = pk;
                wav.write(out.data(), got);
                frames_ += got;
            }
        }
    }
    client->Stop();
    {  // what the resampler still holds
        out.resize(4096 * 2);
        uint8_t* o = reinterpret_cast<uint8_t*>(out.data());
        const int got = swr_convert(swr.get(), &o, 4096, nullptr, 0);
        if (got > 0) {
            wav.write(out.data(), got);
            frames_ += got;
        }
    }
    wav.close();
    running_ = false;
    if (coInit) CoUninitialize();
}

std::vector<std::pair<std::string, std::string>> listCaptureDevices() {
    std::vector<std::pair<std::string, std::string>> out;
    const bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDeviceCollection> col;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                ComPtr<IMMDevice> d;
                if (FAILED(col->Item(i, &d))) continue;
                LPWSTR id = nullptr;
                d->GetId(&id);
                std::string name;
                ComPtr<IPropertyStore> props;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow(v.pwszVal);
                    PropVariantClear(&v);
                }
                out.push_back({narrow(id), name});
                CoTaskMemFree(id);
            }
        }
    }
    if (co) CoUninitialize();
    return out;
}

} // namespace cut
