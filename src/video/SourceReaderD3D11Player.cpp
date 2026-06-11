#include "video/SourceReaderD3D11Player.h"

#include "HResult.h"
#include "Log.h"

SourceReaderD3D11Player::SourceReaderD3D11Player() = default;

SourceReaderD3D11Player::~SourceReaderD3D11Player()
{
    Stop();
}

HRESULT SourceReaderD3D11Player::Start(HWND hwndVideo, const VideoStartOptions& options)
{
    hwndVideo_ = hwndVideo;
    Log::Write(L"SourceReaderD3D11Player fallback requested. Device match='%s'", options.deviceMatch.c_str());

    RETURN_IF_FAILED_LOG(InitializeD3D11(), L"SourceReaderD3D11Player::InitializeD3D11");

    // TODO:
    // 1. Use UvcDeviceEnumerator to select an IMFActivate and create an IMFMediaSource.
    // 2. Create an IMFSourceReader with D3D manager attributes if GPU decode is available.
    // 3. Enumerate native media types and choose MFVideoFormat_H264 or MFVideoFormat_H264_ES.
    // 4. Create/configure the Microsoft H.264 Decoder MFT.
    // 5. Decode into NV12 samples.
    // 6. Create a DXGI swap chain for hwndVideo_.
    // 7. Render NV12 through a D3D11 video processor or pixel shader.
    // 8. Present without CPU readback or per-frame resource creation.

    return E_NOTIMPL;
}

void SourceReaderD3D11Player::Stop()
{
    d3dContext_.Reset();
    d3dDevice_.Reset();
    hwndVideo_ = nullptr;
}

void SourceReaderD3D11Player::Resize(UINT width, UINT height)
{
    Log::Write(L"SourceReaderD3D11Player resize TODO: %ux%u", width, height);
    // TODO: Resize DXGI swap chain buffers and render targets.
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
