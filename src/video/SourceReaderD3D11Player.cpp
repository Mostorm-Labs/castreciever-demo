#include "video/SourceReaderD3D11Player.h"

#include "HResult.h"
#include "Log.h"
#include "StringUtil.h"
#include "device/UvcDeviceEnumerator.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

const wchar_t* VideoSubtypeName(REFGUID subtype)
{
    if (subtype == MFVideoFormat_H264) {
        return L"H264";
    }
    if (subtype == MFVideoFormat_H264_ES) {
        return L"H264_ES";
    }
    if (subtype == MFVideoFormat_RGB32) {
        return L"RGB32";
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

void LogMediaType(const wchar_t* prefix, DWORD typeIndex, IMFMediaType* mediaType)
{
    GUID subtype = GUID_NULL;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 frameRateNumerator = 0;
    UINT32 frameRateDenominator = 0;

    HRESULT hr = mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
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

    Log::Write(L"%s type=%u subtype=%s %s %ux%u fps=%u/%u",
        prefix,
        typeIndex,
        VideoSubtypeName(subtype),
        GuidToString(subtype).c_str(),
        width,
        height,
        frameRateNumerator,
        frameRateDenominator);
}

HRESULT SelectNativeVideoType(IMFSourceReader* reader, bool preferH264, UINT32& width, UINT32& height)
{
    width = 0;
    height = 0;

    Microsoft::WRL::ComPtr<IMFMediaType> firstType;
    Microsoft::WRL::ComPtr<IMFMediaType> selectedType;

    for (DWORD typeIndex = 0;; ++typeIndex) {
        Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
        HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, typeIndex, &mediaType);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            LogHResult(L"IMFSourceReader::GetNativeMediaType", hr);
            return hr;
        }

        LogMediaType(L"SourceReader native", typeIndex, mediaType.Get());

        if (!firstType) {
            firstType = mediaType;
        }

        GUID major = GUID_NULL;
        GUID subtype = GUID_NULL;
        hr = mediaType->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (FAILED(hr)) {
            LogHResult(L"IMFMediaType::GetGUID(MF_MT_MAJOR_TYPE native)", hr);
            continue;
        }

        hr = mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (FAILED(hr)) {
            LogHResult(L"IMFMediaType::GetGUID(MF_MT_SUBTYPE native)", hr);
            continue;
        }

        if (preferH264 &&
            major == MFMediaType_Video &&
            (subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_H264_ES)) {
            selectedType = mediaType;
            break;
        }
    }

    if (!selectedType) {
        selectedType = firstType;
    }

    if (!selectedType) {
        Log::Write(L"SourceReader found no native video media types.");
        return MF_E_INVALIDMEDIATYPE;
    }

    HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, selectedType.Get());
    if (FAILED(hr)) {
        LogHResult(L"IMFSourceReader::SetCurrentMediaType(native)", hr);
        return hr;
    }

    MFGetAttributeSize(selectedType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    Log::Write(L"SourceReader selected native video type: %ux%u", width, height);
    return S_OK;
}

HRESULT ConfigureRgb32Output(IMFSourceReader* reader, UINT32& width, UINT32& height)
{
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    RETURN_IF_FAILED_LOG(MFCreateMediaType(&outputType), L"MFCreateMediaType(SourceReader RGB32)");
    RETURN_IF_FAILED_LOG(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE SourceReader)");
    RETURN_IF_FAILED_LOG(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), L"IMFMediaType::SetGUID(MF_MT_SUBTYPE RGB32 SourceReader)");

    if (width != 0 && height != 0) {
        RETURN_IF_FAILED_LOG(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height), L"MFSetAttributeSize(SourceReader RGB32)");
    }

    RETURN_IF_FAILED_LOG(
        reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get()),
        L"IMFSourceReader::SetCurrentMediaType(RGB32)");

    Microsoft::WRL::ComPtr<IMFMediaType> currentOutputType;
    RETURN_IF_FAILED_LOG(
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentOutputType),
        L"IMFSourceReader::GetCurrentMediaType(RGB32)");

    HRESULT hr = MFGetAttributeSize(currentOutputType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr)) {
        LogHResult(L"MFGetAttributeSize(SourceReader current RGB32)", hr);
        return hr;
    }

    LogMediaType(L"SourceReader output", 0, currentOutputType.Get());
    return S_OK;
}

HRESULT CopySampleToVector(IMFSample* sample, std::vector<BYTE>& bytes)
{
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED_LOG(sample->ConvertToContiguousBuffer(&buffer), L"IMFSample::ConvertToContiguousBuffer(video)");

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    RETURN_IF_FAILED_LOG(buffer->Lock(&data, &maxLength, &currentLength), L"IMFMediaBuffer::Lock(video)");

    bytes.resize(currentLength);
    if (currentLength != 0) {
        std::memcpy(bytes.data(), data, currentLength);
    }

    RETURN_IF_FAILED_LOG(buffer->Unlock(), L"IMFMediaBuffer::Unlock(video)");
    return S_OK;
}

void PaintRgb32Frame(HWND hwnd, const std::vector<BYTE>& bytes, UINT32 width, UINT32 height)
{
    if (hwnd == nullptr || bytes.empty() || width == 0 || height == 0) {
        return;
    }

    RECT client = {};
    if (!GetClientRect(hwnd, &client)) {
        return;
    }

    HDC dc = GetDC(hwnd);
    if (dc == nullptr) {
        return;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        dc,
        0,
        0,
        client.right - client.left,
        client.bottom - client.top,
        0,
        0,
        static_cast<int>(width),
        static_cast<int>(height),
        bytes.data(),
        &bitmapInfo,
        DIB_RGB_COLORS,
        SRCCOPY);

    ReleaseDC(hwnd, dc);
}

} // namespace

SourceReaderD3D11Player::SourceReaderD3D11Player() = default;

SourceReaderD3D11Player::~SourceReaderD3D11Player()
{
    Stop();
}

HRESULT SourceReaderD3D11Player::Start(HWND hwndVideo, const VideoStartOptions& options)
{
    if (hwndVideo == nullptr) {
        return E_INVALIDARG;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return S_OK;
    }

    hwndVideo_ = hwndVideo;
    Log::Write(L"SourceReader fallback requested. Device match='%s'", options.deviceMatch.c_str());

    LOG_IF_FAILED(InitializeD3D11(), L"SourceReaderD3D11Player::InitializeD3D11");

    worker_ = std::thread(&SourceReaderD3D11Player::WorkerThread, this, options);
    return S_OK;
}

void SourceReaderD3D11Player::Stop()
{
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }

    d3dContext_.Reset();
    d3dDevice_.Reset();
    hwndVideo_ = nullptr;
}

void SourceReaderD3D11Player::Resize(UINT width, UINT height)
{
    width_.store(width);
    height_.store(height);
}

void SourceReaderD3D11Player::WorkerThread(VideoStartOptions options)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHResult(L"CoInitializeEx(SourceReader thread)", hr);
        running_.store(false);
        return;
    }

    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource;
    Microsoft::WRL::ComPtr<IMFSourceReader> sourceReader;

    do {
        UvcDeviceEnumerator enumerator;
        UvcDeviceInfo selectedDevice;
        hr = enumerator.FindBestMatch(options.deviceMatch, selectedDevice);
        if (FAILED(hr)) {
            LogHResult(L"UvcDeviceEnumerator::FindBestMatch(SourceReader)", hr);
            break;
        }

        hr = selectedDevice.activate->ActivateObject(IID_PPV_ARGS(&mediaSource));
        if (FAILED(hr)) {
            LogHResult(L"IMFActivate::ActivateObject(IMFMediaSource)", hr);
            break;
        }

        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 3);
        if (FAILED(hr)) {
            LogHResult(L"MFCreateAttributes(SourceReader)", hr);
            break;
        }

        LOG_IF_FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), L"Set MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS");
        LOG_IF_FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), L"Set MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING");

        hr = MFCreateSourceReaderFromMediaSource(mediaSource.Get(), attributes.Get(), &sourceReader);
        if (FAILED(hr)) {
            LogHResult(L"MFCreateSourceReaderFromMediaSource", hr);
            break;
        }

        LOG_IF_FAILED(sourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), L"IMFSourceReader::SetStreamSelection(all false)");
        hr = sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (FAILED(hr)) {
            LogHResult(L"IMFSourceReader::SetStreamSelection(video true)", hr);
            break;
        }

        UINT32 frameWidth = 0;
        UINT32 frameHeight = 0;
        hr = SelectNativeVideoType(sourceReader.Get(), options.preferH264, frameWidth, frameHeight);
        if (FAILED(hr)) {
            break;
        }

        hr = ConfigureRgb32Output(sourceReader.Get(), frameWidth, frameHeight);
        if (FAILED(hr)) {
            break;
        }

        width_.store(frameWidth);
        height_.store(frameHeight);
        Log::Write(L"SourceReader RGB32 diagnostic renderer started: %ux%u", frameWidth, frameHeight);

        DWORD frameCount = 0;
        std::vector<BYTE> frameBytes;

        while (running_.load()) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            Microsoft::WRL::ComPtr<IMFSample> sample;

            hr = sourceReader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                &streamIndex,
                &flags,
                &timestamp,
                &sample);
            if (FAILED(hr)) {
                LogHResult(L"IMFSourceReader::ReadSample(video)", hr);
                break;
            }

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                Log::Write(L"SourceReader video end of stream.");
                break;
            }

            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                hr = ConfigureRgb32Output(sourceReader.Get(), frameWidth, frameHeight);
                if (FAILED(hr)) {
                    break;
                }
                width_.store(frameWidth);
                height_.store(frameHeight);
            }

            if (!sample) {
                Sleep(1);
                continue;
            }

            hr = CopySampleToVector(sample.Get(), frameBytes);
            if (FAILED(hr)) {
                break;
            }

            PaintRgb32Frame(hwndVideo_, frameBytes, frameWidth, frameHeight);

            ++frameCount;
            if (frameCount == 1 || frameCount % 120 == 0) {
                Log::Write(L"SourceReader painted frame %u timestamp=%lld bytes=%zu", frameCount, timestamp, frameBytes.size());
            }
        }
    } while (false);

    if (mediaSource) {
        LOG_IF_FAILED(mediaSource->Shutdown(), L"IMFMediaSource::Shutdown(SourceReader)");
    }

    running_.store(false);
    CoUninitialize();
}

HRESULT SourceReaderD3D11Player::InitializeD3D11()
{
    if (d3dDevice_) {
        return S_OK;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        requestedLevels,
        ARRAYSIZE(requestedLevels),
        D3D11_SDK_VERSION,
        &d3dDevice_,
        &createdLevel,
        &d3dContext_);

#if defined(_DEBUG)
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            requestedLevels,
            ARRAYSIZE(requestedLevels),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &createdLevel,
            &d3dContext_);
    }
#endif

    if (FAILED(hr)) {
        LogHResult(L"D3D11CreateDevice", hr);
        return hr;
    }

    Log::Write(L"D3D11 fallback device created. Feature level=0x%04X", static_cast<unsigned int>(createdLevel));
    return S_OK;
}
