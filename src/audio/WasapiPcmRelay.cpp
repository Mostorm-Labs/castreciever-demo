#include "audio/WasapiPcmRelay.h"

#include "HResult.h"
#include "Log.h"
#include "device/UacDeviceEnumerator.h"

#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr REFERENCE_TIME kBufferDurationHns = 1000000; // 100 ms.

struct CoTaskMemFreeDeleter {
    void operator()(void* value) const
    {
        CoTaskMemFree(value);
    }
};

using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemFreeDeleter>;

struct AudioFormatDescription {
    GUID subtype = GUID_NULL;
    UINT32 channels = 0;
    UINT32 sampleRate = 0;
    UINT32 bitsPerSample = 0;
    UINT32 validBitsPerSample = 0;
    UINT32 blockAlign = 0;
    UINT32 avgBytesPerSecond = 0;
    UINT32 channelMask = 0;
};

bool TryDescribeWaveFormat(const WAVEFORMATEX* format, AudioFormatDescription& description)
{
    if (format == nullptr) {
        return false;
    }

    description.channels = format->nChannels;
    description.sampleRate = format->nSamplesPerSec;
    description.bitsPerSample = format->wBitsPerSample;
    description.validBitsPerSample = format->wBitsPerSample;
    description.blockAlign = format->nBlockAlign;
    description.avgBytesPerSecond = format->nAvgBytesPerSec;

    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        description.subtype = MFAudioFormat_PCM;
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        description.subtype = MFAudioFormat_Float;
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        description.subtype = extensible->SubFormat;
        description.channelMask = extensible->dwChannelMask;
        if (extensible->Samples.wValidBitsPerSample != 0) {
            description.validBitsPerSample = extensible->Samples.wValidBitsPerSample;
        }
    } else {
        return false;
    }

    if (description.channelMask == 0) {
        if (description.channels == 1) {
            description.channelMask = SPEAKER_FRONT_CENTER;
        } else if (description.channels == 2) {
            description.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        }
    }

    return description.subtype == MFAudioFormat_PCM || description.subtype == MFAudioFormat_Float;
}

bool AreWaveFormatsEquivalent(const WAVEFORMATEX* left, const WAVEFORMATEX* right)
{
    AudioFormatDescription leftDescription;
    AudioFormatDescription rightDescription;
    if (!TryDescribeWaveFormat(left, leftDescription) || !TryDescribeWaveFormat(right, rightDescription)) {
        return false;
    }

    return leftDescription.subtype == rightDescription.subtype &&
        leftDescription.channels == rightDescription.channels &&
        leftDescription.sampleRate == rightDescription.sampleRate &&
        leftDescription.bitsPerSample == rightDescription.bitsPerSample &&
        leftDescription.validBitsPerSample == rightDescription.validBitsPerSample &&
        leftDescription.blockAlign == rightDescription.blockAlign;
}

HRESULT CreateAudioMediaType(const WAVEFORMATEX* format, IMFMediaType** mediaType)
{
    if (mediaType == nullptr) {
        return E_POINTER;
    }
    *mediaType = nullptr;

    AudioFormatDescription description;
    if (!TryDescribeWaveFormat(format, description)) {
        Log::Write(L"Unsupported audio format for Media Foundation resampler. formatTag=0x%04X", format ? format->wFormatTag : 0);
        return MF_E_INVALIDMEDIATYPE;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> type;
    RETURN_IF_FAILED_LOG(MFCreateMediaType(&type), L"MFCreateMediaType(audio)");
    RETURN_IF_FAILED_LOG(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), L"IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE audio)");
    RETURN_IF_FAILED_LOG(type->SetGUID(MF_MT_SUBTYPE, description.subtype), L"IMFMediaType::SetGUID(MF_MT_SUBTYPE audio)");
    RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, description.channels), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_NUM_CHANNELS)");
    RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, description.sampleRate), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND)");
    RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, description.blockAlign), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT)");
    RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, description.avgBytesPerSecond), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND)");
    RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, description.bitsPerSample), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE)");
    RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), L"IMFMediaType::SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT audio)");

    if (description.validBitsPerSample != 0 && description.validBitsPerSample != description.bitsPerSample) {
        RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, description.validBitsPerSample), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE)");
    }

    if (description.channelMask != 0) {
        RETURN_IF_FAILED_LOG(type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, description.channelMask), L"IMFMediaType::SetUINT32(MF_MT_AUDIO_CHANNEL_MASK)");
    }

    *mediaType = type.Detach();
    return S_OK;
}

HRESULT CreateSampleFromBytes(const BYTE* data, DWORD byteCount, IMFSample** sample)
{
    if (sample == nullptr) {
        return E_POINTER;
    }
    *sample = nullptr;

    if (data == nullptr || byteCount == 0) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED_LOG(MFCreateMemoryBuffer(byteCount, &buffer), L"MFCreateMemoryBuffer(input audio)");

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    RETURN_IF_FAILED_LOG(buffer->Lock(&destination, &maxLength, nullptr), L"IMFMediaBuffer::Lock(input audio)");

    if (byteCount > maxLength) {
        buffer->Unlock();
        return E_UNEXPECTED;
    }

    if (byteCount != 0) {
        std::memcpy(destination, data, byteCount);
    }

    RETURN_IF_FAILED_LOG(buffer->Unlock(), L"IMFMediaBuffer::Unlock(input audio)");
    RETURN_IF_FAILED_LOG(buffer->SetCurrentLength(byteCount), L"IMFMediaBuffer::SetCurrentLength(input audio)");

    Microsoft::WRL::ComPtr<IMFSample> createdSample;
    RETURN_IF_FAILED_LOG(MFCreateSample(&createdSample), L"MFCreateSample(input audio)");
    RETURN_IF_FAILED_LOG(createdSample->AddBuffer(buffer.Get()), L"IMFSample::AddBuffer(input audio)");

    *sample = createdSample.Detach();
    return S_OK;
}

HRESULT AppendSampleBytes(IMFSample* sample, std::vector<BYTE>& output)
{
    if (sample == nullptr) {
        return E_POINTER;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED_LOG(sample->ConvertToContiguousBuffer(&buffer), L"IMFSample::ConvertToContiguousBuffer(resampler output)");

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    RETURN_IF_FAILED_LOG(buffer->Lock(&data, &maxLength, &currentLength), L"IMFMediaBuffer::Lock(resampler output)");

    const size_t originalSize = output.size();
    output.resize(originalSize + currentLength);
    if (currentLength != 0) {
        std::memcpy(output.data() + originalSize, data, currentLength);
    }

    RETURN_IF_FAILED_LOG(buffer->Unlock(), L"IMFMediaBuffer::Unlock(resampler output)");
    return S_OK;
}

class MfAudioResampler {
public:
    HRESULT Initialize(const WAVEFORMATEX* inputFormat, const WAVEFORMATEX* outputFormat)
    {
        inputFormat_ = {};
        outputFormat_ = {};

        if (!TryDescribeWaveFormat(inputFormat, inputFormat_) ||
            !TryDescribeWaveFormat(outputFormat, outputFormat_)) {
            return MF_E_INVALIDMEDIATYPE;
        }

        Microsoft::WRL::ComPtr<IUnknown> unknown;
        RETURN_IF_FAILED_LOG(
            CoCreateInstance(CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&unknown)),
            L"CoCreateInstance(CLSID_CResamplerMediaObject)");

        Microsoft::WRL::ComPtr<IWMResamplerProps> resamplerProps;
        if (SUCCEEDED(unknown.As(&resamplerProps))) {
            LOG_IF_FAILED(resamplerProps->SetHalfFilterLength(60), L"IWMResamplerProps::SetHalfFilterLength");
        }

        RETURN_IF_FAILED_LOG(unknown.As(&transform_), L"Query IMFTransform from Audio Resampler DSP");

        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        RETURN_IF_FAILED_LOG(CreateAudioMediaType(inputFormat, &inputType), L"CreateAudioMediaType(input)");
        RETURN_IF_FAILED_LOG(CreateAudioMediaType(outputFormat, &outputType), L"CreateAudioMediaType(output)");

        RETURN_IF_FAILED_LOG(transform_->SetInputType(0, inputType.Get(), 0), L"IMFTransform::SetInputType(resampler)");
        RETURN_IF_FAILED_LOG(transform_->SetOutputType(0, outputType.Get(), 0), L"IMFTransform::SetOutputType(resampler)");
        RETURN_IF_FAILED_LOG(transform_->GetOutputStreamInfo(0, &outputStreamInfo_), L"IMFTransform::GetOutputStreamInfo(resampler)");

        LOG_IF_FAILED(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), L"IMFTransform::ProcessMessage(FLUSH resampler)");
        RETURN_IF_FAILED_LOG(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), L"IMFTransform::ProcessMessage(BEGIN_STREAMING resampler)");
        RETURN_IF_FAILED_LOG(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), L"IMFTransform::ProcessMessage(START_OF_STREAM resampler)");

        Log::Write(L"Audio resampler enabled: %u Hz/%u ch/%u-bit -> %u Hz/%u ch/%u-bit",
            inputFormat_.sampleRate,
            inputFormat_.channels,
            inputFormat_.bitsPerSample,
            outputFormat_.sampleRate,
            outputFormat_.channels,
            outputFormat_.bitsPerSample);
        return S_OK;
    }

    HRESULT Process(const BYTE* inputData, UINT32 inputBytes, std::vector<BYTE>& output)
    {
        output.clear();

        if (!transform_) {
            return E_UNEXPECTED;
        }

        Microsoft::WRL::ComPtr<IMFSample> inputSample;
        RETURN_IF_FAILED_LOG(CreateSampleFromBytes(inputData, inputBytes, &inputSample), L"CreateSampleFromBytes(resampler)");

        HRESULT hr = transform_->ProcessInput(0, inputSample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            RETURN_IF_FAILED_LOG(DrainAvailableOutput(output), L"MfAudioResampler::DrainAvailableOutput(before retry)");
            hr = transform_->ProcessInput(0, inputSample.Get(), 0);
        }

        if (FAILED(hr)) {
            LogHResult(L"IMFTransform::ProcessInput(resampler)", hr);
            return hr;
        }

        RETURN_IF_FAILED_LOG(DrainAvailableOutput(output), L"MfAudioResampler::DrainAvailableOutput");
        return S_OK;
    }

private:
    HRESULT DrainAvailableOutput(std::vector<BYTE>& output)
    {
        while (true) {
            const DWORD estimatedOutputBytes =
                static_cast<DWORD>(((static_cast<unsigned long long>(outputFormat_.avgBytesPerSecond) / 10) + outputFormat_.blockAlign - 1) /
                    outputFormat_.blockAlign * outputFormat_.blockAlign);
            const DWORD bufferBytes = std::max<DWORD>(outputStreamInfo_.cbSize, std::max<DWORD>(estimatedOutputBytes, outputFormat_.blockAlign * 1024));

            Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
            RETURN_IF_FAILED_LOG(MFCreateMemoryBuffer(bufferBytes, &outputBuffer), L"MFCreateMemoryBuffer(resampler output)");

            Microsoft::WRL::ComPtr<IMFSample> outputSample;
            RETURN_IF_FAILED_LOG(MFCreateSample(&outputSample), L"MFCreateSample(resampler output)");
            RETURN_IF_FAILED_LOG(outputSample->AddBuffer(outputBuffer.Get()), L"IMFSample::AddBuffer(resampler output)");

            MFT_OUTPUT_DATA_BUFFER outputData = {};
            outputData.dwStreamID = 0;
            outputData.pSample = outputSample.Get();

            DWORD status = 0;
            const HRESULT hr = transform_->ProcessOutput(0, 1, &outputData, &status);
            if (outputData.pEvents != nullptr) {
                outputData.pEvents->Release();
            }

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return S_OK;
            }
            if (FAILED(hr)) {
                LogHResult(L"IMFTransform::ProcessOutput(resampler)", hr);
                return hr;
            }

            RETURN_IF_FAILED_LOG(AppendSampleBytes(outputSample.Get(), output), L"AppendSampleBytes(resampler output)");
        }
    }

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    MFT_OUTPUT_STREAM_INFO outputStreamInfo_ = {};
    AudioFormatDescription inputFormat_;
    AudioFormatDescription outputFormat_;
};

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

UINT32 EstimateRenderFramesForCaptureFrames(UINT32 captureFrames, const WAVEFORMATEX* captureFormat, const WAVEFORMATEX* renderFormat)
{
    if (captureFormat == nullptr || renderFormat == nullptr || captureFormat->nSamplesPerSec == 0) {
        return captureFrames;
    }

    const auto numerator = static_cast<unsigned long long>(captureFrames) * renderFormat->nSamplesPerSec;
    const UINT32 frames = static_cast<UINT32>((numerator + captureFormat->nSamplesPerSec - 1) / captureFormat->nSamplesPerSec);
    return std::max<UINT32>(frames, 1);
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
        Log::Write(L"WasapiPcmRelay::Start skipped because relay is already running.");
        return S_OK;
    }

    Log::Checkpoint(L"WasapiPcmRelay::Start captureDeviceMatch='%s'", captureDeviceMatch.c_str());
    captureDeviceMatch_ = captureDeviceMatch;

    {
        std::lock_guard<std::mutex> lock(initMutex_);
        initDone_ = false;
        initHr_ = E_PENDING;
    }

    Log::Checkpoint(L"WasapiPcmRelay starting worker thread");
    worker_ = std::thread(&WasapiPcmRelay::WorkerThread, this);

    HRESULT startHr = E_FAIL;
    {
        std::unique_lock<std::mutex> lock(initMutex_);
        initCondition_.wait(lock, [this]() { return initDone_; });
        startHr = initHr_;
    }

    if (FAILED(startHr)) {
        LogHResult(L"WasapiPcmRelay worker initialization", startHr);
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
    } else {
        Log::Write(L"WasapiPcmRelay::Start completed.");
    }

    return startHr;
}

void WasapiPcmRelay::Stop()
{
    Log::Checkpoint(L"WasapiPcmRelay::Stop");
    running_.store(false);
    if (worker_.joinable()) {
        Log::Write(L"Joining WASAPI worker thread.");
        worker_.join();
        Log::Write(L"WASAPI worker thread joined.");
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
    Log::Checkpoint(L"WASAPI worker thread entered");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHResult(L"CoInitializeEx(audio thread)", hr);
        SignalInitialized(hr);
        running_.store(false);
        return;
    }
    Log::Write(L"WASAPI worker COM initialized.");

    DWORD avrtTaskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrtTaskIndex);
    if (avrtHandle == nullptr) {
        LogHResult(L"AvSetMmThreadCharacteristicsW", HResultFromLastError());
    } else {
        Log::Write(L"WASAPI worker joined MMCSS Pro Audio task. handle=0x%p taskIndex=%lu", avrtHandle, avrtTaskIndex);
    }

    Microsoft::WRL::ComPtr<IAudioClient> captureAudioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    Microsoft::WRL::ComPtr<IAudioClient> renderAudioClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    UINT32 renderBufferFrames = 0;
    WaveFormatPtr captureFormat;
    WaveFormatPtr renderFormat;
    MfAudioResampler resampler;
    bool useResampler = false;

    do {
        UacDeviceEnumerator uacEnumerator;
        UacDeviceInfo captureDeviceInfo;
        Log::Checkpoint(L"UacDeviceEnumerator::FindBestMatch");
        hr = uacEnumerator.FindBestMatch(captureDeviceMatch_, captureDeviceInfo);
        if (FAILED(hr)) {
            LogHResult(L"UacDeviceEnumerator::FindBestMatch", hr);
            break;
        }
        Log::Write(L"Selected UAC capture endpoint for relay. name='%s' id='%s'",
            captureDeviceInfo.friendlyName.c_str(),
            captureDeviceInfo.deviceId.c_str());

        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> mmEnumerator;
        Log::Checkpoint(L"CoCreateInstance(MMDeviceEnumerator render)");
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&mmEnumerator));
        if (FAILED(hr)) {
            LogHResult(L"CoCreateInstance(MMDeviceEnumerator render)", hr);
            break;
        }

        Microsoft::WRL::ComPtr<IMMDevice> renderDevice;
        Log::Checkpoint(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender/eConsole)");
        hr = mmEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
        if (FAILED(hr)) {
            LogHResult(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender/eConsole)", hr);
            break;
        }

        Log::Checkpoint(L"IMMDevice::Activate(capture IAudioClient)");
        hr = captureDeviceInfo.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &captureAudioClient);
        if (FAILED(hr)) {
            LogHResult(L"IMMDevice::Activate(capture IAudioClient)", hr);
            break;
        }
        Log::Write(L"Capture IAudioClient activated. client=0x%p", captureAudioClient.Get());

        Log::Checkpoint(L"IMMDevice::Activate(render IAudioClient)");
        hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &renderAudioClient);
        if (FAILED(hr)) {
            LogHResult(L"IMMDevice::Activate(render IAudioClient)", hr);
            break;
        }
        Log::Write(L"Render IAudioClient activated. client=0x%p", renderAudioClient.Get());

        WAVEFORMATEX* rawCaptureFormat = nullptr;
        Log::Checkpoint(L"IAudioClient::GetMixFormat(capture)");
        hr = captureAudioClient->GetMixFormat(&rawCaptureFormat);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetMixFormat(capture)", hr);
            break;
        }
        captureFormat.reset(rawCaptureFormat);
        LogWaveFormat(L"UAC capture format", captureFormat.get());

        WAVEFORMATEX* rawRenderFormat = nullptr;
        Log::Checkpoint(L"IAudioClient::GetMixFormat(render)");
        hr = renderAudioClient->GetMixFormat(&rawRenderFormat);
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::GetMixFormat(render)", hr);
            break;
        }
        renderFormat.reset(rawRenderFormat);
        LogWaveFormat(L"Default render mix format", renderFormat.get());

        WAVEFORMATEX* closestFormat = nullptr;
        hr = renderAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, renderFormat.get(), &closestFormat);
        if (closestFormat != nullptr) {
            LogWaveFormat(L"Closest render format", closestFormat);
            CoTaskMemFree(closestFormat);
        }

        if (hr == S_FALSE) {
            Log::Write(L"Default render endpoint does not support its own mix format.");
            hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            break;
        }
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::IsFormatSupported(render)", hr);
            break;
        }

        useResampler = !AreWaveFormatsEquivalent(captureFormat.get(), renderFormat.get());
        if (useResampler) {
            hr = resampler.Initialize(captureFormat.get(), renderFormat.get());
            if (FAILED(hr)) {
                LogHResult(L"MfAudioResampler::Initialize", hr);
                break;
            }
        } else {
            Log::Write(L"Capture and render formats match. WASAPI relay will write PCM directly.");
        }

        Log::Checkpoint(L"IAudioClient::Initialize(capture)");
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

        Log::Checkpoint(L"IAudioClient::Initialize(render)");
        hr = renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            kBufferDurationHns,
            0,
            renderFormat.get(),
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

        Log::Checkpoint(L"IAudioClient::Start(render)");
        hr = renderAudioClient->Start();
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::Start(render)", hr);
            break;
        }

        Log::Checkpoint(L"IAudioClient::Start(capture)");
        hr = captureAudioClient->Start();
        if (FAILED(hr)) {
            LogHResult(L"IAudioClient::Start(capture)", hr);
            LOG_IF_FAILED(renderAudioClient->Stop(), L"IAudioClient::Stop(render after capture start failure)");
            break;
        }

        SignalInitialized(S_OK);
        Log::Checkpoint(L"WASAPI PCM relay started");
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
                if (useResampler) {
                    if (silent) {
                        const UINT32 renderFrames = EstimateRenderFramesForCaptureFrames(framesAvailable, captureFormat.get(), renderFormat.get());
                        hr = CopyCaptureToRender(
                            renderClient.Get(),
                            renderAudioClient.Get(),
                            renderBufferFrames,
                            nullptr,
                            renderFrames,
                            renderFormat->nBlockAlign,
                            true,
                            running_);
                    } else {
                        std::vector<BYTE> convertedAudio;
                        hr = resampler.Process(
                            captureData,
                            framesAvailable * captureFormat->nBlockAlign,
                            convertedAudio);
                        if (SUCCEEDED(hr) && !convertedAudio.empty()) {
                            const UINT32 convertedFrames = static_cast<UINT32>(convertedAudio.size() / renderFormat->nBlockAlign);
                            hr = CopyCaptureToRender(
                                renderClient.Get(),
                                renderAudioClient.Get(),
                                renderBufferFrames,
                                convertedAudio.data(),
                                convertedFrames,
                                renderFormat->nBlockAlign,
                                false,
                                running_);
                        }
                    }
                } else {
                    hr = CopyCaptureToRender(
                        renderClient.Get(),
                        renderAudioClient.Get(),
                        renderBufferFrames,
                        silent ? nullptr : captureData,
                        framesAvailable,
                        renderFormat->nBlockAlign,
                        silent,
                        running_);
                }

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
