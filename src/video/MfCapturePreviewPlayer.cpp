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

HRESULT MfCapturePreviewPlayer::Start(HWND hwndVideo, const std::wstring& deviceMatch)
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
    RETURN_IF_FAILED_LOG(enumerator.FindBestMatch(deviceMatch, selectedDevice), L"UvcDeviceEnumerator::FindBestMatch");

    auto* callback = new CaptureEngineCallback();
    callback_.Attach(callback);

    RETURN_IF_FAILED_LOG(
        CoCreateInstance(CLSID_MFCaptureEngine, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&captureEngine_)),
        L"CoCreateInstance(CLSID_MFCaptureEngine)");

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

    LOG_IF_FAILED(ConfigureH264IfAvailable(), L"MfCapturePreviewPlayer::ConfigureH264IfAvailable");
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

    DWORD previewSinkStreamIndex = 0;
    hr = previewSink_->AddStream(
        static_cast<DWORD>(MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_VIDEO_PREVIEW),
        nullptr,
        nullptr,
        &previewSinkStreamIndex);
    if (FAILED(hr)) {
        LogHResult(L"IMFCapturePreviewSink::AddStream(video preview)", hr);
        Stop();
        return hr;
    }

    Log::Write(L"Preview sink stream index=%u", previewSinkStreamIndex);

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
}

void MfCapturePreviewPlayer::Resize(UINT width, UINT height)
{
    if (hwndVideo_ != nullptr) {
        InvalidateRect(hwndVideo_, nullptr, FALSE);
    }

    Log::Write(L"Video resize: %ux%u", width, height);
}

HRESULT MfCapturePreviewPlayer::ConfigureH264IfAvailable()
{
    if (!captureEngine_) {
        return E_UNEXPECTED;
    }

    Microsoft::WRL::ComPtr<IMFCaptureSource> source;
    RETURN_IF_FAILED_LOG(captureEngine_->GetSource(&source), L"IMFCaptureEngine::GetSource");

    DWORD streamCount = 0;
    RETURN_IF_FAILED_LOG(source->GetDeviceStreamCount(&streamCount), L"IMFCaptureSource::GetDeviceStreamCount");

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
                hr = source->SetCurrentDeviceMediaType(streamIndex, mediaType.Get());
                if (SUCCEEDED(hr)) {
                    Log::Write(L"Selected H.264 UVC media type on stream %u type %u.", streamIndex, typeIndex);
                    return S_OK;
                }

                LogHResult(L"IMFCaptureSource::SetCurrentDeviceMediaType(H.264)", hr);
            }
        }
    }

    Log::Write(L"No explicit H.264 UVC media type selected. Capture Engine will auto-negotiate preview.");
    return S_FALSE;
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
