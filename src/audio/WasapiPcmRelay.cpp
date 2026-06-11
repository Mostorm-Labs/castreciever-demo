#include "audio/WasapiPcmRelay.h"

#include "HResult.h"
#include "Log.h"
#include "device/UacDeviceEnumerator.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

constexpr REFERENCE_TIME kBufferDurationHns = 1000000; // 100 ms.

struct CoTaskMemFreeDeleter {
    void operator()(void* value) const
    {
        CoTaskMemFree(value);
    }
};

using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemFreeDeleter>;

void LogWaveFormat(const wchar_t* prefix, const WAVEFORMATEX* format)
{
    if (format == nullptr) {
        Log::Write(L"%s: <null format>", prefix);
        return;
    }

    WORD validBits = format->wBitsPerSample;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->Samples.wValidBitsPerSample != 0) {
            validBits = extensible->Samples.wValidBitsPerSample;
        }
    }

    Log::Write(L"%s: sampleRate=%u channels=%u bits=%u blockAlign=%u formatTag=0x%04X",
        prefix,
        format->nSamplesPerSec,
        format->nChannels,
        validBits,
        format->nBlockAlign,
        format->wFormatTag);
}

HRESULT CopyCaptureToRender(
    IAudioRenderClient* renderClient,
    IAudioClient* renderAudioClient,
    UINT32 renderBufferFrames,
    const BYTE* captureData,
    UINT32 captureFrames,
    UINT32 blockAlign,
    bool writeSilence,
    std::atomic_bool& running)
{
    UINT32 framesWritten = 0;

    while (framesWritten < captureFrames && running.load()) {
        UINT32 paddingFrames = 0;
        HRESULT hr = renderAudioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetCurrentPadding(render)", hr);
            return hr;
        }

        const UINT32 availableFrames = paddingFrames < renderBufferFrames ? renderBufferFrames - paddingFrames : 0;
        if (availableFrames == 0) {
            Sleep(1);
            continue;
        }

        const UINT32 framesToWrite = std::min(captureFrames - framesWritten, availableFrames);
        BYTE* renderData = nullptr;
        hr = renderClient->GetBuffer(framesToWrite, &renderData);
        if (FAILED(hr)) {
            LogHResult(L"IAudioRenderClient::GetBuffer", hr);
            return hr;
        }

        DWORD renderFlags = 0;
        if (writeSilence || captureData == nullptr) {
            renderFlags = AUDCLNT_BUFFERFLAGS_SILENT;
        } else {
            const BYTE* source = captureData + static_cast<size_t>(framesWritten) * blockAlign;
            std::memcpy(renderData, source, static_cast<size_t>(framesToWrite) * blockAlign);
        }

        hr = renderClient->ReleaseBuffer(framesToWrite, renderFlags);
        if (FAILED(hr)) {
            LogHResult(L"IAudioRenderClient::ReleaseBuffer", hr);
            return hr;
        }

        framesWritten += framesToWrite;
    }

    return S_OK;
}

} // namespace

WasapiPcmRelay::WasapiPcmRelay() = default;

WasapiPcmRelay::~WasapiPcmRelay()
{
    Stop();
}

HRESULT WasapiPcmRelay::Start(const std::wstring& captureDeviceMatch)
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return S_OK;
    }

    captureDeviceMatch_ = captureDeviceMatch;

    {
        std::lock_guard<std::mutex> lock(initMutex_);
        initDone_ = false;
        initHr_ = E_PENDING;
    }

    worker_ = std::thread(&WasapiPcmRelay::WorkerThread, this);

    HRESULT startHr = E_FAIL;
    {
        std::unique_lock<std::mutex> lock(initMutex_);
        initCondition_.wait(lock, [this]() { return initDone_; });
        startHr = initHr_;
    }

    if (FAILED(startHr)) {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    return startHr;
}

void WasapiPcmRelay::Stop()
{
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void WasapiPcmRelay::SetMuted(bool muted)
{
    muted_.store(muted);
    Log::Write(L"Audio muted=%s", muted ? L"true" : L"false");
}

bool WasapiPcmRelay::IsMuted() const
{
    return muted_.load();
}

void WasapiPcmRelay::SignalInitialized(HRESULT hr)
{
    {
        std::lock_guard<std::mutex> lock(initMutex_);
        initHr_ = hr;
        initDone_ = true;
    }
    initCondition_.notify_all();
}

static bool IsInitializationSignalPending(std::mutex& mutex, bool& initDone)
{
    std::lock_guard<std::mutex> lock(mutex);
    return !initDone;
}

void WasapiPcmRelay::WorkerThread()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHResult(L"CoInitializeEx(audio thread)", hr);
        SignalInitialized(hr);
        running_.store(false);
        return;
    }

    DWORD avrtTaskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrtTaskIndex);
    if (avrtHandle == nullptr) {
        LogHResult(L"AvSetMmThreadCharacteristicsW", HResultFromLastError());
    }

    Microsoft::WRL::ComPtr<IAudioClient> captureAudioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    Microsoft::WRL::ComPtr<IAudioClient> renderAudioClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    UINT32 renderBufferFrames = 0;
    WaveFormatPtr captureFormat;

    do {
        UacDeviceEnumerator uacEnumerator;
        UacDeviceInfo captureDeviceInfo;
        hr = uacEnumerator.FindBestMatch(captureDeviceMatch_, captureDeviceInfo);
        if (FAILED(hr)) {
            LogHResult(L"UacDeviceEnumerator::FindBestMatch", hr);
            break;
        }

        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> mmEnumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&mmEnumerator));
        if (FAILED(hr)) {
            LogHResult(L"CoCreateInstance(MMDeviceEnumerator render)", hr);
            break;
        }

        Microsoft::WRL::ComPtr<IMMDevice> renderDevice;
        hr = mmEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
        if (FAILED(hr)) {
            LogHResult(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender/eConsole)", hr);
            break;
        }

        hr = captureDeviceInfo.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &captureAudioClient);
        if (FAILED(hr)) {
            LogHResult(L"IMMDevice::Activate(capture IAudioClient)", hr);
            break;
        }

        hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &renderAudioClient);
        if (FAILED(hr)) {
            LogHResult(L"IMMDevice::Activate(render IAudioClient)", hr);
            break;
        }

        WAVEFORMATEX* rawCaptureFormat = nullptr;
        hr = captureAudioClient->GetMixFormat(&rawCaptureFormat);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetMixFormat(capture)", hr);
            break;
        }
        captureFormat.reset(rawCaptureFormat);
        LogWaveFormat(L"UAC capture format", captureFormat.get());

        WAVEFORMATEX* closestFormat = nullptr;
        hr = renderAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, captureFormat.get(), &closestFormat);
        if (closestFormat != nullptr) {
            LogWaveFormat(L"Closest render format", closestFormat);
            CoTaskMemFree(closestFormat);
        }

        if (hr == S_FALSE) {
            Log::Write(L"Default render endpoint does not support the capture PCM format. Resampler is not implemented in this PoC.");
            hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            break;
        }
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::IsFormatSupported(render)", hr);
            break;
        }

        hr = captureAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            kBufferDurationHns,
            0,
            captureFormat.get(),
            nullptr);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::Initialize(capture)", hr);
            break;
        }

        hr = renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            kBufferDurationHns,
            0,
            captureFormat.get(),
            nullptr);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::Initialize(render)", hr);
            break;
        }

        hr = captureAudioClient->GetService(IID_PPV_ARGS(&captureClient));
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetService(IAudioCaptureClient)", hr);
            break;
        }

        hr = renderAudioClient->GetService(IID_PPV_ARGS(&renderClient));
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetService(IAudioRenderClient)", hr);
            break;
        }

        hr = renderAudioClient->GetBufferSize(&renderBufferFrames);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetBufferSize(render)", hr);
            break;
        }

        BYTE* initialSilence = nullptr;
        hr = renderClient->GetBuffer(renderBufferFrames, &initialSilence);
        if (FAILED(hr)) {
            LogHResult(L"IAudioRenderClient::GetBuffer(initial silence)", hr);
            break;
        }

        hr = renderClient->ReleaseBuffer(renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) {
            LogHResult(L"IAudioRenderClient::ReleaseBuffer(initial silence)", hr);
            break;
        }

        hr = renderAudioClient->Start();
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::Start(render)", hr);
            break;
        }

        hr = captureAudioClient->Start();
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::Start(capture)", hr);
            LOG_IF_FAILED(renderAudioClient->Stop(), L"IAudioClient::Stop(render after capture start failure)");
            break;
        }

        SignalInitialized(S_OK);
        Log::Write(L"WASAPI PCM relay started.");

        while (running_.load()) {
            UINT32 packetFrames = 0;
            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                LogHResult(L"IAudioCaptureClient::GetNextPacketSize", hr);
                break;
            }

            if (packetFrames == 0) {
                Sleep(5);
                continue;
            }

            while (packetFrames != 0 && running_.load()) {
                BYTE* captureData = nullptr;
                UINT32 framesAvailable = 0;
                DWORD captureFlags = 0;

                hr = captureClient->GetBuffer(
                    &captureData,
                    &framesAvailable,
                    &captureFlags,
                    nullptr,
                    nullptr);
                if (FAILED(hr)) {
                    LogHResult(L"IAudioCaptureClient::GetBuffer", hr);
                    running_.store(false);
                    break;
                }

                const bool silent = muted_.load() || (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                hr = CopyCaptureToRender(
                    renderClient.Get(),
                    renderAudioClient.Get(),
                    renderBufferFrames,
                    silent ? nullptr : captureData,
                    framesAvailable,
                    captureFormat->nBlockAlign,
                    silent,
                    running_);

                const HRESULT releaseHr = captureClient->ReleaseBuffer(framesAvailable);
                if (FAILED(releaseHr)) {
                    LogHResult(L"IAudioCaptureClient::ReleaseBuffer", releaseHr);
                    if (SUCCEEDED(hr)) {
                        hr = releaseHr;
                    }
                }

                if (FAILED(hr)) {
                    running_.store(false);
                    break;
                }

                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) {
                    LogHResult(L"IAudioCaptureClient::GetNextPacketSize(loop)", hr);
                    running_.store(false);
                    break;
                }
            }
        }

        LOG_IF_FAILED(captureAudioClient->Stop(), L"IAudioClient::Stop(capture)");
        LOG_IF_FAILED(renderAudioClient->Stop(), L"IAudioClient::Stop(render)");
        Log::Write(L"WASAPI PCM relay stopped.");
    } while (false);

    if (FAILED(hr) && IsInitializationSignalPending(initMutex_, initDone_)) {
        SignalInitialized(hr);
        running_.store(false);
    }

    if (avrtHandle != nullptr) {
        if (!AvRevertMmThreadCharacteristics(avrtHandle)) {
            LogHResult(L"AvRevertMmThreadCharacteristics", HResultFromLastError());
        }
    }

    CoUninitialize();
}
