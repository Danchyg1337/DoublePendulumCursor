#include "LoopbackCapture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <chrono>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr int    PAD_BLOCK = 480;          // pad silence in ~10 ms blocks
constexpr double SILENCE_AFTER = 0.05;     // s without packets -> pad silence
constexpr double DEVICE_CHECK_SEC = 3.0;   // follow the default device

// Default render endpoint id (caller frees with CoTaskMemFree), or nullptr.
LPWSTR defaultDeviceId(IMMDeviceEnumerator* en) {
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) return nullptr;
    LPWSTR id = nullptr;
    dev->GetId(&id);
    dev->Release();
    return id;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

LoopbackCapture::LoopbackCapture(bpm::AudioRing& ring) : ring_(ring) {
    thread_ = std::thread([this] { run(); });
}

LoopbackCapture::~LoopbackCapture() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void LoopbackCapture::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_) {
        if (!captureOnce()) {
            for (int i = 0; i < 100 && running_; ++i) sleepMs(10);   // retry in 1 s
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

bool LoopbackCapture::captureOnce() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX*        fmt = nullptr;
    LPWSTR               devId = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&en)))) break;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
        if (FAILED(dev->GetId(&devId))) break;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&client)))) break;
        if (FAILED(client->GetMixFormat(&fmt)) || !fmt) break;

        // sample format of the shared-mode mix (normally 32-bit float)
        WORD tag = fmt->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
            tag = static_cast<WORD>(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat.Data1);
        const int  bits = fmt->wBitsPerSample;
        const int  ch = fmt->nChannels;
        const int  frameBytes = fmt->nBlockAlign;
        const bool isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32);
        const bool isPcm = (tag == WAVE_FORMAT_PCM && (bits == 16 || bits == 24 || bits == 32));
        if (!(isFloat || isPcm) || ch <= 0) break;
        const int sr = static_cast<int>(fmt->nSamplesPerSec);

        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      1000000 /* 100 ms */, 0, fmt, nullptr))) break;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&cap)))) break;

        ring_.restart(sr);
        if (FAILED(client->Start())) break;

        std::vector<float> mono;
        const std::vector<float> zeros(PAD_BLOCK, 0.0f);
        double lastData = bpm::nowSec();
        double padFrom = lastData;
        double lastCheck = lastData;
        bool failed = false;

        while (running_) {
            sleepMs(5);
            bool got = false;
            UINT32 pkt = 0;
            if (FAILED(cap->GetNextPacketSize(&pkt))) { failed = true; break; }
            while (pkt > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { failed = true; break; }
                mono.assign(frames, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    for (UINT32 f = 0; f < frames; ++f) {
                        const BYTE* p = data + static_cast<std::size_t>(f) * frameBytes;
                        double s = 0;
                        for (int c = 0; c < ch; ++c) {
                            if (isFloat) {
                                float v; std::memcpy(&v, p + c * 4, 4); s += v;
                            } else if (bits == 16) {
                                std::int16_t v; std::memcpy(&v, p + c * 2, 2); s += v / 32768.0;
                            } else if (bits == 24) {
                                const BYTE* q = p + c * 3;
                                std::int32_t v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24);
                                s += v / 2147483648.0;
                            } else {
                                std::int32_t v; std::memcpy(&v, p + c * 4, 4); s += v / 2147483648.0;
                            }
                        }
                        mono[f] = static_cast<float>(s / ch);   // channel mean
                    }
                }
                ring_.push(mono.data(), mono.size(), bpm::nowSec());
                cap->ReleaseBuffer(frames);
                got = true;
                if (FAILED(cap->GetNextPacketSize(&pkt))) { failed = true; break; }
            }
            if (failed) break;

            const double now = bpm::nowSec();
            if (got) {
                lastData = padFrom = now;
            } else if (now - lastData > SILENCE_AFTER) {
                // nothing is playing: Windows sends no packets, keep the clock going
                while ((now - padFrom) * sr >= PAD_BLOCK) {
                    ring_.push(zeros.data(), zeros.size(), now);
                    padFrom += static_cast<double>(PAD_BLOCK) / sr;
                }
            }
            if (now - lastCheck > DEVICE_CHECK_SEC) {            // follow default device
                lastCheck = now;
                LPWSTR cur = defaultDeviceId(en);
                const bool changed = !cur || std::wcscmp(cur, devId) != 0;
                if (cur) CoTaskMemFree(cur);
                if (changed) break;
            }
        }
        client->Stop();
        ok = !failed;
    } while (false);

    if (fmt) CoTaskMemFree(fmt);
    if (devId) CoTaskMemFree(devId);
    safeRelease(cap);
    safeRelease(client);
    safeRelease(dev);
    safeRelease(en);
    return ok;
}

// ---- BeatThread ---------------------------------------------------------------
BeatThread::BeatThread(bpm::BeatWorker& worker) : worker_(worker) {
    thread_ = std::thread([this] {
        const int slices = static_cast<int>(bpm::BeatWorker::UPDATE_SEC * 100);
        while (running_) {
            for (int i = 0; i < slices && running_; ++i) sleepMs(10);
            if (!running_) break;
            worker_.step();
        }
    });
}

BeatThread::~BeatThread() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}
