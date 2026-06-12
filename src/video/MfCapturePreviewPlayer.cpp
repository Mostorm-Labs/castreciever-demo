#include "video/MfCapturePreviewPlayer.h"

#include "HResult.h"
#include "StringUtil.h"
#include "device/UvcDeviceEnumerator.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <strsafe.h>

namespace {

const wchar_t* VideoSubtypeName(REFGUID subtype)
{
    if (subtype == MFVideoFormat_H264) {
        return L"H264";
    }
    if (subtype == MFVideoFormat_H264_ES) {
        return L"H264_ES";
    }
    if (subtype == MFVideoFormat_NV12) {
        return L"NV12";
    }
    if (subtype == MFVideoFormat_YUY2) {
        return L"YUY2";
    }
    if (subtype == MFVideoFormat_MJPG) {
        return L"MJPG";
    }
    return L"Unknown";
}

bool IsVideoStreamCategory(MF_CAPTURE_ENGINE_STREAM_CATEGORY category)
{
    return category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_VIDEO_PREVIEW ||
        category == MF_CAPTURE_ENGINE_STREAM_CATEGORY_VIDEO_CAPTURE;
}

bool FrameRateMatchesTarget(UINT32 numerator, UINT32 denominator, UINT32 targetFps)
{
    if (targetFps == 0) {
        return true;
    }

    if (numerator == 0 || denominator == 0) {
        return false;
    }

    const double fps = static_cast<double>(numerator) / static_cast<double>(denominator);
    const double target = static_cast<double>(targetFps);
    const double delta = fps > target ? fps - target : target - fps;
    return delta < 0.25;
}

HRESULT CreateCaptureEngine(Microsoft::WRL::ComPtr<IMFCaptureEngine>& captureEngine)
{
    Microsoft::WRL::ComPtr<IMFCaptureEngineClassFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_MFCaptureEngineClassFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));

    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(CLSID_MFCaptureEngine, IID_PPV_ARGS(&captureEngine));
        if (SUCCEEDED(hr)) {
            Log::Write(L"Created Capture Engine through IMFCaptureEngineClassFactory.");
            return S_OK;
        }

        LogHResult(L"IMFCaptureEngineClassFactory::CreateInstance(CLSID_MFCaptureEngine)", hr);
    } else {
        LogHResult(L"CoCreateInstance(CLSID_MFCaptureEngineClassFactory)", hr);
    }

    hr = CoCreateInstance(
        CLSID_MFCaptureEngine,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&captureEngine));

    if (SUCCEEDED(hr)) {
        Log::Write(L"Created Capture Engine directly through CLSID_MFCaptureEngine.");
        return S_OK;
    }

    LogHResult(L"CoCreateInstance(CLSID_MFCaptureEngine)", hr);
    return hr;
}

void LogMediaType(DWORD streamIndex, DWORD typeIndex, IMFMediaType* mediaType)
{
    GUID major = GUID_NULL;
    GUID subtype = GUID_NULL;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 frameRateNumerator = 0;
    UINT32 frameRateDenominator = 0;

    HRESULT hr = mediaType->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr)) {
        LogHResult(L"IMFMediaType::GetGUID(MF_MT_MAJOR_TYPE)", hr);
    }

    hr = mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr)) {
        LogHResult(L"IMFMediaType::GetGUID(MF_MT_SUBTYPE)", hr);
    }

    hr = MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) && hr != MF_E_ATTRIBUTENOTFOUND) {
        LogHResult(L"MFGetAttributeSize(MF_MT_FRAME_SIZE)", hr);
    }

    hr = MFGetAttributeRatio(mediaType, MF_MT_FRAME_RATE, &frameRateNumerator, &frameRateDenominator);
    if (FAILED(hr) && hr != MF_E_ATTRIBUTENOTFOUND) {
        LogHResult(L"MFGetAttributeRatio(MF_MT_FRAME_RATE)", hr);
    }

    Log::Write(L"Capture type stream=%u type=%u subtype=%s %s %ux%u fps=%u/%u",
        streamIndex,
        typeIndex,
        VideoSubtypeName(subtype),
        GuidToString(subtype).c_str(),
        width,
        height,
        frameRateNumerator,
        frameRateDenominator);
}

} // namespace

class MfCapturePreviewPlayer::CaptureEngineCallback final : public IMFCaptureEngineOnEventCallback {
public:
    CaptureEngineCallback()
        : initializedEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    ~CaptureEngineCallback()
    {
        if (initializedEvent_ != nullptr) {
            CloseHandle(initializedEvent_);
        }
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }

        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IMFCaptureEngineOnEventCallback)) {
            *object = static_cast<IMFCaptureEngineOnEventCallback*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refCount_);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    STDMETHODIMP OnEvent(IMFMediaEvent* event) override
    {
        if (event == nullptr) {
            return E_POINTER;
        }

        MediaEventType eventType = MEUnknown;
        HRESULT status = S_OK;
        GUID extendedType = GUID_NULL;
        HRESULT hr = event->GetType(&eventType);
        if (FAILED(hr)) {
            LogHResult(L"IMFMediaEvent::GetType", hr);
            return S_OK;
        }

        hr = event->GetStatus(&status);
        if (FAILED(hr)) {
            LogHResult(L"IMFMediaEvent::GetStatus", hr);
            status = hr;
        }

        hr = event->GetExtendedType(&extendedType);
        if (FAILED(hr) && hr != MF_E_ATTRIBUTENOTFOUND) {
            LogHResult(L"IMFMediaEvent::GetExtendedType", hr);
        }

        if (FAILED(status)) {
            LogHResult(L"Capture Engine event status", status);
        } else {
            Log::Write(L"Capture Engine event: type=%d extended=%s status=0x%08X",
                static_cast<int>(eventType),
                GuidToString(extendedType).c_str(),
                static_cast<unsigned int>(status));
        }

        if (eventType == MEExtendedType && extendedType == MF_CAPTURE_ENGINE_INITIALIZED) {
            initializedStatus_ = status;
            if (initializedEvent_ != nullptr) {
                SetEvent(initializedEvent_);
            }
        }

        return S_OK;
    }

    HRESULT WaitForInitialized(DWORD timeoutMs)
    {
        if (initializedEvent_ == nullptr) {
            return E_HANDLE;
        }

        const DWORD waitResult = WaitForSingleObject(initializedEvent_, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            return initializedStatus_;
        }
        if (waitResult == WAIT_TIMEOUT) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        return HResultFromLastError();
    }

private:
    volatile long refCount_ = 1;
    HANDLE initializedEvent_ = nullptr;
    HRESULT initializedStatus_ = E_PENDING;
};

MfCapturePreviewPlayer::MfCapturePreviewPlayer() = default;

MfCapturePreviewPlayer::~MfCapturePreviewPlayer()
{
    Stop();
}

HRESULT MfCapturePreviewPlayer::Start(HWND hwndVideo, const VideoStartOptions& options)
{
    if (started_) {
        return S_OK;
    }

    if (hwndVideo == nullptr) {
        return E_INVALIDARG;
    }

    hwndVideo_ = hwndVideo;

    UvcDeviceEnumerator enumerator;
    UvcDeviceInfo selectedDevice;
    RETURN_IF_FAILED_LOG(enumerator.FindBestMatch(options.deviceMatch, selectedDevice), L"UvcDeviceEnumerator::FindBestMatch");

    auto* callback = new CaptureEngineCallback();
    callback_.Attach(callback);

    RETURN_IF_FAILED_LOG(CreateCaptureEngine(captureEngine_), L"CreateCaptureEngine");

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    RETURN_IF_FAILED_LOG(MFCreateAttributes(&attributes, 1), L"MFCreateAttributes(Capture Engine)");

    HRESULT hr = captureEngine_->Initialize(callback_.Get(), attributes.Get(), nullptr, selectedDevice.activate.Get());
    if (FAILED(hr)) {
        LogHResult(L"IMFCaptureEngine::Initialize", hr);
        Stop();
        return hr;
    }

    hr = callback->WaitForInitialized(10000);
    if (FAILED(hr)) {
        LogHResult(L"Waiting for MF_CAPTURE_ENGINE_INITIALIZED", hr);
        Stop();
        return hr;
    }

    LOG_IF_FAILED(ConfigureVideoMediaType(options.preferH264, options.targetVideoFps), L"MfCapturePreviewPlayer::ConfigureVideoMediaType");
    LogCurrentVideoTypes();

    Microsoft::WRL::ComPtr<IMFCaptureSink> sink;
    hr = captureEngine_->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW, &sink);
    if (FAILED(hr)) {
        LogHResult(L"IMFCaptureEngine::GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW)", hr);
        Stop();
        return hr;
    }

    hr = sink.As(&previewSink_);
    if (FAILED(hr)) {
        LogHResult(L"Query IMFCapturePreviewSink", hr);
        Stop();
        return hr;
    }

    hr = previewSink_->SetRenderHandle(hwndVideo_);
    if (FAILED(hr)) {
        LogHResult(L"IMFCapturePreviewSink::SetRenderHandle", hr);
        Stop();
        return hr;
    }

    hr = ConfigurePreviewSink(options.previewSinkMode);
    if (FAILED(hr)) {
        LogHResult(L"MfCapturePreviewPlayer::ConfigurePreviewSink", hr);
        Stop();
        return hr;
    }

    hr = captureEngine_->StartPreview();
    if (FAILED(hr)) {
        LogHResult(L"IMFCaptureEngine::StartPreview", hr);
        Stop();
        return hr;
    }

    started_ = true;
    Log::Write(L"Capture Engine preview started.");
    return S_OK;
}

void MfCapturePreviewPlayer::Stop()
{
    if (captureEngine_ && started_) {
        LOG_IF_FAILED(captureEngine_->StopPreview(), L"IMFCaptureEngine::StopPreview");
    }

    started_ = false;
    previewSink_.Reset();
    captureEngine_.Reset();
    callback_.Reset();
    hwndVideo_ = nullptr;
    previewSourceStreamIndex_ = 0;
    hasPreviewSourceStreamIndex_ = false;
}

void MfCapturePreviewPlayer::Resize(UINT width, UINT height)
{
    if (hwndVideo_ != nullptr) {
        InvalidateRect(hwndVideo_, nullptr, FALSE);
    }

    Log::Write(L"Video resize: %ux%u", width, height);
}

HRESULT MfCapturePreviewPlayer::ConfigureVideoMediaType(bool preferH264, UINT32 targetVideoFps)
{
    if (!captureEngine_) {
        return E_UNEXPECTED;
    }

    Microsoft::WRL::ComPtr<IMFCaptureSource> source;
    RETURN_IF_FAILED_LOG(captureEngine_->GetSource(&source), L"IMFCaptureEngine::GetSource");

    DWORD streamCount = 0;
    RETURN_IF_FAILED_LOG(source->GetDeviceStreamCount(&streamCount), L"IMFCaptureSource::GetDeviceStreamCount");

    bool sawVideoStream = false;
    DWORD firstVideoStreamIndex = 0;
    Microsoft::WRL::ComPtr<IMFMediaType> firstH264Type;
    DWORD firstH264StreamIndex = 0;
    DWORD firstH264TypeIndex = 0;

    for (DWORD streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
        MF_CAPTURE_ENGINE_STREAM_CATEGORY category = MF_CAPTURE_ENGINE_STREAM_CATEGORY_UNSUPPORTED;
        HRESULT hr = source->GetDeviceStreamCategory(streamIndex, &category);
        if (FAILED(hr)) {
            LogHResult(L"IMFCaptureSource::GetDeviceStreamCategory", hr);
            continue;
        }

        if (!IsVideoStreamCategory(category)) {
            continue;
        }

        if (!sawVideoStream) {
            sawVideoStream = true;
            firstVideoStreamIndex = streamIndex;
            previewSourceStreamIndex_ = streamIndex;
            hasPreviewSourceStreamIndex_ = true;
        }

        for (DWORD typeIndex = 0;; ++typeIndex) {
            Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
            hr = source->GetAvailableDeviceMediaType(streamIndex, typeIndex, &mediaType);
            if (hr == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (FAILED(hr)) {
                LogHResult(L"IMFCaptureSource::GetAvailableDeviceMediaType", hr);
                break;
            }

            LogMediaType(streamIndex, typeIndex, mediaType.Get());

            if (!preferH264) {
                continue;
            }

            GUID major = GUID_NULL;
            GUID subtype = GUID_NULL;
            hr = mediaType->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (FAILED(hr)) {
                LogHResult(L"IMFMediaType::GetGUID(MF_MT_MAJOR_TYPE)", hr);
                continue;
            }

            hr = mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (FAILED(hr)) {
                LogHResult(L"IMFMediaType::GetGUID(MF_MT_SUBTYPE)", hr);
                continue;
            }

            if (major == MFMediaType_Video &&
                (subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_H264_ES)) {
                if (!firstH264Type) {
                    firstH264Type = mediaType;
                    firstH264StreamIndex = streamIndex;
                    firstH264TypeIndex = typeIndex;
                }

                UINT32 frameRateNumerator = 0;
                UINT32 frameRateDenominator = 0;
                hr = MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &frameRateNumerator, &frameRateDenominator);
                if (FAILED(hr) && hr != MF_E_ATTRIBUTENOTFOUND) {
                    LogHResult(L"MFGetAttributeRatio(MF_MT_FRAME_RATE Capture H.264)", hr);
                }

                if (targetVideoFps != 0 && !FrameRateMatchesTarget(frameRateNumerator, frameRateDenominator, targetVideoFps)) {
                    continue;
                }

                hr = source->SetCurrentDeviceMediaType(streamIndex, mediaType.Get());
                if (FAILED(hr)) {
                    LogHResult(L"IMFCaptureSource::SetCurrentDeviceMediaType(H.264)", hr);
                    continue;
                }

                previewSourceStreamIndex_ = streamIndex;
                hasPreviewSourceStreamIndex_ = true;
                Log::Write(L"Selected H.264 UVC media type on stream %u type %u fps=%u/%u.",
                    streamIndex,
                    typeIndex,
                    frameRateNumerator,
                    frameRateDenominator);
                return S_OK;
            }
        }
    }

    if (preferH264 && firstH264Type) {
        if (targetVideoFps != 0) {
            Log::Write(L"No Capture Engine H.264 media type matched --video-fps %u. Falling back to first H.264 type.", targetVideoFps);
        }

        const HRESULT hr = source->SetCurrentDeviceMediaType(firstH264StreamIndex, firstH264Type.Get());
        if (SUCCEEDED(hr)) {
            previewSourceStreamIndex_ = firstH264StreamIndex;
            hasPreviewSourceStreamIndex_ = true;
            Log::Write(L"Selected H.264 UVC media type on stream %u type %u.", firstH264StreamIndex, firstH264TypeIndex);
            return S_OK;
        }

        LogHResult(L"IMFCaptureSource::SetCurrentDeviceMediaType(first H.264)", hr);
    }

    if (sawVideoStream) {
        previewSourceStreamIndex_ = firstVideoStreamIndex;
        hasPreviewSourceStreamIndex_ = true;
        if (preferH264) {
            Log::Write(L"No explicit H.264 UVC media type selected. Using first video source stream %u and allowing Capture Engine to auto-negotiate.", firstVideoStreamIndex);
        } else {
            Log::Write(L"Video format selection set to auto. Using first video source stream %u with Capture Engine default media type.", firstVideoStreamIndex);
        }
        return S_FALSE;
    }

    Log::Write(L"No explicit H.264 UVC media type selected. Capture Engine will auto-negotiate preview.");
    return MF_E_INVALIDSTREAMNUMBER;
}

HRESULT MfCapturePreviewPlayer::ConfigurePreviewSink(PreviewSinkMode mode)
{
    if (!previewSink_) {
        return E_UNEXPECTED;
    }

    if (mode == PreviewSinkMode::Default) {
        Log::Write(L"Preview sink mode=default. Using SetRenderHandle only; Capture Engine will configure preview streams.");
        return S_OK;
    }

    if (!hasPreviewSourceStreamIndex_) {
        Log::Write(L"No video source stream was selected for preview.");
        return MF_E_INVALIDSTREAMNUMBER;
    }

    LOG_IF_FAILED(previewSink_->RemoveAllStreams(), L"IMFCapturePreviewSink::RemoveAllStreams");

    DWORD previewSinkStreamIndex = 0;
    if (mode == PreviewSinkMode::AutoAddStream) {
        HRESULT hr = previewSink_->AddStream(
            previewSourceStreamIndex_,
            nullptr,
            nullptr,
            &previewSinkStreamIndex);
        if (FAILED(hr)) {
            LogHResult(L"IMFCapturePreviewSink::AddStream(auto video preview)", hr);
            return hr;
        }

        Log::Write(L"Preview sink accepted auto AddStream. source stream=%u sink stream=%u", previewSourceStreamIndex_, previewSinkStreamIndex);
        return S_OK;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> previewMediaType;
    HRESULT hr = CreatePreviewMediaType(&previewMediaType);
    if (FAILED(hr)) {
        LogHResult(L"MfCapturePreviewPlayer::CreatePreviewMediaType", hr);
        return hr;
    }

    hr = previewSink_->AddStream(
        previewSourceStreamIndex_,
        previewMediaType.Get(),
        nullptr,
        &previewSinkStreamIndex);
    if (FAILED(hr)) {
        LogHResult(L"IMFCapturePreviewSink::AddStream(RGB32 video preview)", hr);
        return hr;
    }

    Log::Write(L"Preview sink accepted RGB32 AddStream. source stream=%u sink stream=%u", previewSourceStreamIndex_, previewSinkStreamIndex);
    return S_OK;
}

HRESULT MfCapturePreviewPlayer::CreatePreviewMediaType(IMFMediaType** mediaType)
{
    if (mediaType == nullptr) {
        return E_POINTER;
    }
    *mediaType = nullptr;

    if (!captureEngine_ || !hasPreviewSourceStreamIndex_) {
        return E_UNEXPECTED;
    }

    Microsoft::WRL::ComPtr<IMFCaptureSource> source;
    RETURN_IF_FAILED_LOG(captureEngine_->GetSource(&source), L"IMFCaptureEngine::GetSource(CreatePreviewMediaType)");

    Microsoft::WRL::ComPtr<IMFMediaType> currentType;
    RETURN_IF_FAILED_LOG(
        source->GetCurrentDeviceMediaType(previewSourceStreamIndex_, &currentType),
        L"IMFCaptureSource::GetCurrentDeviceMediaType(CreatePreviewMediaType)");

    Microsoft::WRL::ComPtr<IMFMediaType> previewType;
    RETURN_IF_FAILED_LOG(MFCreateMediaType(&previewType), L"MFCreateMediaType(preview)");

    RETURN_IF_FAILED_LOG(currentType->CopyAllItems(previewType.Get()), L"IMFMediaType::CopyAllItems(preview)");
    RETURN_IF_FAILED_LOG(previewType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE preview)");
    RETURN_IF_FAILED_LOG(previewType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), L"IMFMediaType::SetGUID(MF_MT_SUBTYPE RGB32 preview)");
    RETURN_IF_FAILED_LOG(previewType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), L"IMFMediaType::SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT preview)");

    Log::Write(L"Created preview sink media type RGB32 for source stream %u.", previewSourceStreamIndex_);
    *mediaType = previewType.Detach();
    return S_OK;
}

void MfCapturePreviewPlayer::LogCurrentVideoTypes()
{
    if (!captureEngine_) {
        return;
    }

    Microsoft::WRL::ComPtr<IMFCaptureSource> source;
    HRESULT hr = captureEngine_->GetSource(&source);
    if (FAILED(hr)) {
        LogHResult(L"IMFCaptureEngine::GetSource(LogCurrentVideoTypes)", hr);
        return;
    }

    DWORD streamCount = 0;
    hr = source->GetDeviceStreamCount(&streamCount);
    if (FAILED(hr)) {
        LogHResult(L"IMFCaptureSource::GetDeviceStreamCount(LogCurrentVideoTypes)", hr);
        return;
    }

    for (DWORD streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
        MF_CAPTURE_ENGINE_STREAM_CATEGORY category = MF_CAPTURE_ENGINE_STREAM_CATEGORY_UNSUPPORTED;
        hr = source->GetDeviceStreamCategory(streamIndex, &category);
        if (FAILED(hr) || !IsVideoStreamCategory(category)) {
            continue;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> currentType;
        hr = source->GetCurrentDeviceMediaType(streamIndex, &currentType);
        if (SUCCEEDED(hr) && currentType) {
            LogMediaType(streamIndex, 0, currentType.Get());
        }
    }
}
