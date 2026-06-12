#include "video/SourceReaderD3D11Player.h"

#include "HResult.h"
#include "Log.h"
#include "StringUtil.h"
#include "device/UvcDeviceEnumerator.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <avrt.h>
#include <d3d10.h>

#include <algorithm>

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

HRESULT ConfigureNv12Output(IMFSourceReader* reader, UINT32& width, UINT32& height)
{
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    RETURN_IF_FAILED_LOG(MFCreateMediaType(&outputType), L"MFCreateMediaType(SourceReader NV12)");
    RETURN_IF_FAILED_LOG(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE SourceReader)");
    RETURN_IF_FAILED_LOG(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), L"IMFMediaType::SetGUID(MF_MT_SUBTYPE NV12 SourceReader)");

    if (width != 0 && height != 0) {
        RETURN_IF_FAILED_LOG(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height), L"MFSetAttributeSize(SourceReader NV12)");
    }

    RETURN_IF_FAILED_LOG(
        reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get()),
        L"IMFSourceReader::SetCurrentMediaType(NV12)");

    Microsoft::WRL::ComPtr<IMFMediaType> currentOutputType;
    RETURN_IF_FAILED_LOG(
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentOutputType),
        L"IMFSourceReader::GetCurrentMediaType(NV12)");

    HRESULT hr = MFGetAttributeSize(currentOutputType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr)) {
        LogHResult(L"MFGetAttributeSize(SourceReader current NV12)", hr);
        return hr;
    }

    LogMediaType(L"SourceReader output", 0, currentOutputType.Get());
    return S_OK;
}

void PaintDiagnosticPattern(HWND hwnd, const wchar_t* message)
{
    if (hwnd == nullptr) {
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

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const COLORREF colors[] = {
        RGB(32, 32, 32),
        RGB(200, 30, 30),
        RGB(30, 150, 50),
        RGB(30, 90, 210),
        RGB(220, 190, 40),
    };

    for (int i = 0; i < static_cast<int>(ARRAYSIZE(colors)); ++i) {
        RECT band = client;
        band.left = i * width / static_cast<int>(ARRAYSIZE(colors));
        band.right = (i + 1) * width / static_cast<int>(ARRAYSIZE(colors));
        HBRUSH brush = CreateSolidBrush(colors[i]);
        if (brush != nullptr) {
            FillRect(dc, &band, brush);
            DeleteObject(brush);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(
        28,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr) {
        oldFont = SelectObject(dc, font);
    }

    RECT textRect = client;
    textRect.left += 24;
    textRect.right -= 24;
    textRect.top += 24;
    DrawTextW(dc, message, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }

    ReleaseDC(hwnd, dc);
}

void PaintDiagnosticPatternOnVideoAndParent(HWND hwnd, const wchar_t* message)
{
    PaintDiagnosticPattern(hwnd, message);
    PaintDiagnosticPattern(GetParent(hwnd), message);
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
    Log::Write(L"SourceReader backend selected. Device match='%s'", options.deviceMatch.c_str());
    PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader backend starting...");

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

    ResetRenderResources();
    hwndVideo_ = nullptr;
}

void SourceReaderD3D11Player::Resize(UINT width, UINT height)
{
    width_.store(width);
    height_.store(height);
}

HRESULT SourceReaderD3D11Player::InitializeDxgiDeviceManager()
{
    RETURN_IF_FAILED_LOG(InitializeD3D11(), L"SourceReaderD3D11Player::InitializeD3D11");

    if (dxgiDeviceManager_) {
        return S_OK;
    }

    RETURN_IF_FAILED_LOG(
        MFCreateDXGIDeviceManager(&dxgiDeviceManagerResetToken_, &dxgiDeviceManager_),
        L"MFCreateDXGIDeviceManager(SourceReader)");
    RETURN_IF_FAILED_LOG(
        dxgiDeviceManager_->ResetDevice(d3dDevice_.Get(), dxgiDeviceManagerResetToken_),
        L"IMFDXGIDeviceManager::ResetDevice(SourceReader)");

    Log::Write(L"SourceReader D3D11 device manager initialized for DXVA output.");
    return S_OK;
}

HRESULT SourceReaderD3D11Player::EnsureRenderResources(UINT32 frameWidth, UINT32 frameHeight)
{
    RETURN_IF_FAILED_LOG(InitializeD3D11(), L"SourceReaderD3D11Player::InitializeD3D11");

    RECT client = {};
    if (!GetClientRect(hwndVideo_, &client)) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"GetClientRect(video hwnd D3D11)", hr);
        return hr;
    }

    const UINT clientWidth = static_cast<UINT>(std::max<LONG>(0, client.right - client.left));
    const UINT clientHeight = static_cast<UINT>(std::max<LONG>(0, client.bottom - client.top));
    RETURN_IF_FAILED_LOG(EnsureSwapChain(clientWidth, clientHeight), L"SourceReaderD3D11Player::EnsureSwapChain");
    if (clientWidth == 0 || clientHeight == 0) {
        return S_OK;
    }

    RETURN_IF_FAILED_LOG(EnsureVideoProcessor(frameWidth, frameHeight), L"SourceReaderD3D11Player::EnsureVideoProcessor");
    return S_OK;
}

HRESULT SourceReaderD3D11Player::EnsureSwapChain(UINT clientWidth, UINT clientHeight)
{
    if (clientWidth == 0 || clientHeight == 0) {
        return S_OK;
    }

    if (!swapChain_) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        RETURN_IF_FAILED_LOG(d3dDevice_.As(&dxgiDevice), L"ID3D11Device::QueryInterface(IDXGIDevice)");

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        RETURN_IF_FAILED_LOG(dxgiDevice->GetAdapter(&adapter), L"IDXGIDevice::GetAdapter");

        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        RETURN_IF_FAILED_LOG(adapter->GetParent(IID_PPV_ARGS(&factory)), L"IDXGIAdapter::GetParent(IDXGIFactory2)");

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = clientWidth;
        swapChainDesc.Height = clientHeight;
        swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 2;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        HRESULT hr = factory->CreateSwapChainForHwnd(
            d3dDevice_.Get(),
            hwndVideo_,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain_);
        if (FAILED(hr)) {
            LogHResult(L"IDXGIFactory2::CreateSwapChainForHwnd(SourceReader)", hr);
            return hr;
        }

        LOG_IF_FAILED(factory->MakeWindowAssociation(hwndVideo_, DXGI_MWA_NO_ALT_ENTER), L"IDXGIFactory2::MakeWindowAssociation");
        swapChainWidth_ = clientWidth;
        swapChainHeight_ = clientHeight;
        Log::Write(L"SourceReader D3D11 swap chain created: %ux%u", clientWidth, clientHeight);
    } else if (clientWidth != swapChainWidth_ || clientHeight != swapChainHeight_) {
        videoOutputView_.Reset();
        swapChainBackBuffer_.Reset();

        HRESULT hr = swapChain_->ResizeBuffers(0, clientWidth, clientHeight, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LogHResult(L"IDXGISwapChain::ResizeBuffers(SourceReader)", hr);
            return hr;
        }

        swapChainWidth_ = clientWidth;
        swapChainHeight_ = clientHeight;
        Log::Write(L"SourceReader D3D11 swap chain resized: %ux%u", clientWidth, clientHeight);
    }

    if (!swapChainBackBuffer_) {
        RETURN_IF_FAILED_LOG(swapChain_->GetBuffer(0, IID_PPV_ARGS(&swapChainBackBuffer_)), L"IDXGISwapChain::GetBuffer(SourceReader)");
    }

    return S_OK;
}

HRESULT SourceReaderD3D11Player::EnsureVideoProcessor(UINT32 frameWidth, UINT32 frameHeight)
{
    if (frameWidth == 0 || frameHeight == 0 || swapChainWidth_ == 0 || swapChainHeight_ == 0 || !swapChainBackBuffer_) {
        return E_INVALIDARG;
    }

    if (!videoDevice_) {
        RETURN_IF_FAILED_LOG(d3dDevice_.As(&videoDevice_), L"ID3D11Device::QueryInterface(ID3D11VideoDevice)");
    }

    if (!videoContext_) {
        RETURN_IF_FAILED_LOG(d3dContext_.As(&videoContext_), L"ID3D11DeviceContext::QueryInterface(ID3D11VideoContext)");
    }

    const bool needsNewProcessor =
        !videoProcessorEnumerator_ ||
        !videoProcessor_ ||
        videoProcessorInputWidth_ != frameWidth ||
        videoProcessorInputHeight_ != frameHeight ||
        videoProcessorOutputWidth_ != swapChainWidth_ ||
        videoProcessorOutputHeight_ != swapChainHeight_;

    if (needsNewProcessor) {
        videoOutputView_.Reset();
        videoProcessor_.Reset();
        videoProcessorEnumerator_.Reset();
        inputViewCache_.clear();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = frameWidth;
        contentDesc.InputHeight = frameHeight;
        contentDesc.InputFrameRate.Numerator = 60;
        contentDesc.InputFrameRate.Denominator = 1;
        contentDesc.OutputWidth = swapChainWidth_;
        contentDesc.OutputHeight = swapChainHeight_;
        contentDesc.OutputFrameRate.Numerator = 60;
        contentDesc.OutputFrameRate.Denominator = 1;
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        RETURN_IF_FAILED_LOG(
            videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnumerator_),
            L"ID3D11VideoDevice::CreateVideoProcessorEnumerator(SourceReader)");
        RETURN_IF_FAILED_LOG(
            videoDevice_->CreateVideoProcessor(videoProcessorEnumerator_.Get(), 0, &videoProcessor_),
            L"ID3D11VideoDevice::CreateVideoProcessor(SourceReader)");

        videoProcessorInputWidth_ = frameWidth;
        videoProcessorInputHeight_ = frameHeight;
        videoProcessorOutputWidth_ = swapChainWidth_;
        videoProcessorOutputHeight_ = swapChainHeight_;

        Log::Write(L"SourceReader D3D11 video processor created: input=%ux%u output=%ux%u",
            frameWidth,
            frameHeight,
            swapChainWidth_,
            swapChainHeight_);
    }

    if (!videoOutputView_) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
        outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputViewDesc.Texture2D.MipSlice = 0;

        RETURN_IF_FAILED_LOG(
            videoDevice_->CreateVideoProcessorOutputView(
                swapChainBackBuffer_.Get(),
                videoProcessorEnumerator_.Get(),
                &outputViewDesc,
                &videoOutputView_),
            L"ID3D11VideoDevice::CreateVideoProcessorOutputView(SourceReader)");
    }

    return S_OK;
}

HRESULT SourceReaderD3D11Player::RenderNv12Sample(IMFSample* sample, UINT32 frameWidth, UINT32 frameHeight, DWORD& byteCount)
{
    byteCount = 0;
    RETURN_IF_FAILED_LOG(EnsureRenderResources(frameWidth, frameHeight), L"SourceReaderD3D11Player::EnsureRenderResources");

    if (!swapChain_ || !videoProcessor_ || !videoOutputView_) {
        return S_OK;
    }

    if (sample == nullptr) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        LogHResult(L"IMFSample::GetBufferByIndex(video)", hr);
        return hr;
    }

    LOG_IF_FAILED(buffer->GetCurrentLength(&byteCount), L"IMFMediaBuffer::GetCurrentLength(video)");

    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
    hr = buffer.As(&dxgiBuffer);
    if (FAILED(hr)) {
        LogHResult(L"IMFMediaBuffer::QueryInterface(IMFDXGIBuffer)", hr);
        Log::Write(L"SourceReader sample is not a DXGI buffer; DXVA zero-copy output is not active.");
        return hr;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    RETURN_IF_FAILED_LOG(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)), L"IMFDXGIBuffer::GetResource(ID3D11Texture2D)");

    UINT subresourceIndex = 0;
    RETURN_IF_FAILED_LOG(dxgiBuffer->GetSubresourceIndex(&subresourceIndex), L"IMFDXGIBuffer::GetSubresourceIndex");

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    RETURN_IF_FAILED_LOG(GetNv12InputView(texture.Get(), subresourceIndex, &inputView), L"SourceReaderD3D11Player::GetNv12InputView");
    RETURN_IF_FAILED_LOG(DrawNv12Frame(inputView.Get(), frameWidth, frameHeight), L"SourceReaderD3D11Player::DrawNv12Frame");
    return S_OK;
}

HRESULT SourceReaderD3D11Player::GetNv12InputView(
    ID3D11Texture2D* texture,
    UINT subresourceIndex,
    ID3D11VideoProcessorInputView** inputView)
{
    if (texture == nullptr || inputView == nullptr || !videoDevice_ || !videoProcessorEnumerator_) {
        return E_INVALIDARG;
    }

    for (const auto& cached : inputViewCache_) {
        if (cached.texture.Get() == texture && cached.subresourceIndex == subresourceIndex) {
            return cached.inputView.CopyTo(inputView);
        }
    }

    D3D11_TEXTURE2D_DESC textureDesc = {};
    texture->GetDesc(&textureDesc);
    if (textureDesc.Format != DXGI_FORMAT_NV12) {
        Log::Write(L"SourceReader DXGI sample texture format is %u, expected NV12 (%u).",
            static_cast<unsigned int>(textureDesc.Format),
            static_cast<unsigned int>(DXGI_FORMAT_NV12));
    }

    const UINT mipLevels = std::max<UINT>(1, textureDesc.MipLevels);
    const UINT mipSlice = subresourceIndex % mipLevels;
    const UINT arraySlice = subresourceIndex / mipLevels;
    if (arraySlice >= textureDesc.ArraySize) {
        Log::Write(L"Invalid NV12 texture subresource: subresource=%u mipLevels=%u arraySize=%u",
            subresourceIndex,
            mipLevels,
            textureDesc.ArraySize);
        return E_INVALIDARG;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
    inputViewDesc.FourCC = 0;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = mipSlice;
    inputViewDesc.Texture2D.ArraySlice = arraySlice;

    CachedInputView cached;
    cached.texture = texture;
    cached.subresourceIndex = subresourceIndex;
    RETURN_IF_FAILED_LOG(
        videoDevice_->CreateVideoProcessorInputView(
            texture,
            videoProcessorEnumerator_.Get(),
            &inputViewDesc,
            &cached.inputView),
        L"ID3D11VideoDevice::CreateVideoProcessorInputView(SourceReader)");

    Log::Write(L"Cached NV12 DXVA input view: texture=%ux%u format=%u subresource=%u arraySlice=%u",
        textureDesc.Width,
        textureDesc.Height,
        static_cast<unsigned int>(textureDesc.Format),
        subresourceIndex,
        arraySlice);

    inputViewCache_.push_back(cached);
    if (inputViewCache_.size() > 32) {
        inputViewCache_.erase(inputViewCache_.begin());
    }

    return inputViewCache_.back().inputView.CopyTo(inputView);
}

HRESULT SourceReaderD3D11Player::DrawNv12Frame(ID3D11VideoProcessorInputView* inputView, UINT32 frameWidth, UINT32 frameHeight)
{
    if (inputView == nullptr || !videoContext_ || !videoProcessor_ || !videoOutputView_ || !swapChain_) {
        return E_INVALIDARG;
    }

    RECT sourceRect = {
        0,
        0,
        static_cast<LONG>(frameWidth),
        static_cast<LONG>(frameHeight),
    };
    RECT destinationRect = {
        0,
        0,
        static_cast<LONG>(swapChainWidth_),
        static_cast<LONG>(swapChainHeight_),
    };

    videoContext_->VideoProcessorSetOutputTargetRect(videoProcessor_.Get(), TRUE, &destinationRect);
    videoContext_->VideoProcessorSetStreamFrameFormat(videoProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &destinationRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView;

    HRESULT hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), videoOutputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        LogHResult(L"ID3D11VideoContext::VideoProcessorBlt(SourceReader)", hr);
        return hr;
    }

    hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) {
        LogHResult(L"IDXGISwapChain::Present(SourceReader)", hr);
        return hr;
    }

    return S_OK;
}

void SourceReaderD3D11Player::ResetRenderResources()
{
    if (d3dContext_) {
        d3dContext_->ClearState();
        d3dContext_->Flush();
    }

    inputViewCache_.clear();
    videoOutputView_.Reset();
    videoProcessor_.Reset();
    videoProcessorEnumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    swapChainBackBuffer_.Reset();
    swapChain_.Reset();
    dxgiDeviceManager_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    dxgiDeviceManagerResetToken_ = 0;
    swapChainWidth_ = 0;
    swapChainHeight_ = 0;
    videoProcessorInputWidth_ = 0;
    videoProcessorInputHeight_ = 0;
    videoProcessorOutputWidth_ = 0;
    videoProcessorOutputHeight_ = 0;
}

void SourceReaderD3D11Player::WorkerThread(VideoStartOptions options)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHResult(L"CoInitializeEx(SourceReader thread)", hr);
        PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: CoInitializeEx");
        running_.store(false);
        return;
    }

    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Playback", &mmcssTaskIndex);
    if (mmcssHandle == nullptr) {
        LogHResult(L"AvSetMmThreadCharacteristicsW(SourceReader Playback)", HResultFromLastError());
    }
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        LogHResult(L"SetThreadPriority(SourceReader)", HResultFromLastError());
    }

    PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader opening UVC device...");

    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource;
    Microsoft::WRL::ComPtr<IMFSourceReader> sourceReader;

    do {
        UvcDeviceEnumerator enumerator;
        UvcDeviceInfo selectedDevice;
        hr = enumerator.FindBestMatch(options.deviceMatch, selectedDevice);
        if (FAILED(hr)) {
            LogHResult(L"UvcDeviceEnumerator::FindBestMatch(SourceReader)", hr);
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: UVC device not found");
            break;
        }

        hr = selectedDevice.activate->ActivateObject(IID_PPV_ARGS(&mediaSource));
        if (FAILED(hr)) {
            LogHResult(L"IMFActivate::ActivateObject(IMFMediaSource)", hr);
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: ActivateObject(IMFMediaSource)");
            break;
        }

        hr = InitializeDxgiDeviceManager();
        if (FAILED(hr)) {
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: D3D11 device manager");
            break;
        }

        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 4);
        if (FAILED(hr)) {
            LogHResult(L"MFCreateAttributes(SourceReader)", hr);
            break;
        }

        LOG_IF_FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), L"Set MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS");
        LOG_IF_FAILED(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), L"Set MF_LOW_LATENCY");
        hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiDeviceManager_.Get());
        if (FAILED(hr)) {
            LogHResult(L"Set MF_SOURCE_READER_D3D_MANAGER", hr);
            break;
        }

        hr = MFCreateSourceReaderFromMediaSource(mediaSource.Get(), attributes.Get(), &sourceReader);
        if (FAILED(hr)) {
            LogHResult(L"MFCreateSourceReaderFromMediaSource", hr);
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: MFCreateSourceReaderFromMediaSource");
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
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: selecting native video type");
            break;
        }

        hr = ConfigureNv12Output(sourceReader.Get(), frameWidth, frameHeight);
        if (FAILED(hr)) {
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: configuring NV12 DXVA decoder output");
            break;
        }

        width_.store(frameWidth);
        height_.store(frameHeight);
        Log::Write(L"SourceReader D3D11 NV12/DXVA zero-copy renderer started: %ux%u", frameWidth, frameHeight);

        DWORD frameCount = 0;
        DWORD statsFrameCount = 0;
        ULONGLONG statsStart = GetTickCount64();

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
                PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: ReadSample(video)");
                break;
            }

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                Log::Write(L"SourceReader video end of stream.");
                break;
            }

            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                hr = ConfigureNv12Output(sourceReader.Get(), frameWidth, frameHeight);
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

            DWORD sampleByteCount = 0;
            hr = RenderNv12Sample(sample.Get(), frameWidth, frameHeight, sampleByteCount);
            if (FAILED(hr)) {
                PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: rendering decoded NV12 DXVA frame");
                break;
            }

            ++frameCount;
            ++statsFrameCount;
            if (frameCount == 1 || statsFrameCount >= 120) {
                const ULONGLONG now = GetTickCount64();
                const ULONGLONG elapsedMs = std::max<ULONGLONG>(1, now - statsStart);
                const double fps = static_cast<double>(statsFrameCount) * 1000.0 / static_cast<double>(elapsedMs);
                Log::Write(L"SourceReader painted frame %u timestamp=%lld bytes=%u fps=%.1f",
                    frameCount,
                    timestamp,
                    sampleByteCount,
                    fps);
                statsFrameCount = 0;
                statsStart = now;
            }
        }
    } while (false);

    if (mediaSource) {
        LOG_IF_FAILED(mediaSource->Shutdown(), L"IMFMediaSource::Shutdown(SourceReader)");
    }

    running_.store(false);
    if (mmcssHandle != nullptr) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
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

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(d3dDevice_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    Log::Write(L"D3D11 device created for SourceReader renderer. Feature level=0x%04X", static_cast<unsigned int>(createdLevel));
    return S_OK;
}
