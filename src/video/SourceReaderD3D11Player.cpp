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
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

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

constexpr char kFullscreenTriangleShader[] = R"(
Texture2D frameTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 uvs[3] = {
        float2(0.0,  1.0),
        float2(0.0, -1.0),
        float2(2.0,  1.0)
    };

    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    return frameTexture.Sample(linearSampler, input.uv);
}
)";

HRESULT CompileShader(const char* entryPoint, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& blob)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        kFullscreenTriangleShader,
        std::strlen(kFullscreenTriangleShader),
        nullptr,
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        &blob,
        &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
            OutputDebugStringA("\r\n");
        }
        LogHResult(L"D3DCompile(SourceReader renderer shader)", hr);
        return hr;
    }

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

HRESULT SourceReaderD3D11Player::EnsureRenderResources(UINT32 frameWidth, UINT32 frameHeight)
{
    RETURN_IF_FAILED_LOG(InitializeD3D11(), L"SourceReaderD3D11Player::InitializeD3D11");
    RETURN_IF_FAILED_LOG(EnsureShaders(), L"SourceReaderD3D11Player::EnsureShaders");
    RETURN_IF_FAILED_LOG(EnsureFrameTexture(frameWidth, frameHeight), L"SourceReaderD3D11Player::EnsureFrameTexture");

    RECT client = {};
    if (!GetClientRect(hwndVideo_, &client)) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"GetClientRect(video hwnd D3D11)", hr);
        return hr;
    }

    const UINT clientWidth = static_cast<UINT>(std::max<LONG>(0, client.right - client.left));
    const UINT clientHeight = static_cast<UINT>(std::max<LONG>(0, client.bottom - client.top));
    RETURN_IF_FAILED_LOG(EnsureSwapChain(clientWidth, clientHeight), L"SourceReaderD3D11Player::EnsureSwapChain");
    return S_OK;
}

HRESULT SourceReaderD3D11Player::EnsureShaders()
{
    if (vertexShader_ && pixelShader_ && sampler_) {
        return S_OK;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertexShaderBlob;
    RETURN_IF_FAILED_LOG(CompileShader("VSMain", "vs_4_0", vertexShaderBlob), L"CompileShader(VSMain)");
    RETURN_IF_FAILED_LOG(
        d3dDevice_->CreateVertexShader(
            vertexShaderBlob->GetBufferPointer(),
            vertexShaderBlob->GetBufferSize(),
            nullptr,
            &vertexShader_),
        L"ID3D11Device::CreateVertexShader(SourceReader)");

    Microsoft::WRL::ComPtr<ID3DBlob> pixelShaderBlob;
    RETURN_IF_FAILED_LOG(CompileShader("PSMain", "ps_4_0", pixelShaderBlob), L"CompileShader(PSMain)");
    RETURN_IF_FAILED_LOG(
        d3dDevice_->CreatePixelShader(
            pixelShaderBlob->GetBufferPointer(),
            pixelShaderBlob->GetBufferSize(),
            nullptr,
            &pixelShader_),
        L"ID3D11Device::CreatePixelShader(SourceReader)");

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    RETURN_IF_FAILED_LOG(
        d3dDevice_->CreateSamplerState(&samplerDesc, &sampler_),
        L"ID3D11Device::CreateSamplerState(SourceReader)");

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
        ID3D11RenderTargetView* nullRenderTarget = nullptr;
        d3dContext_->OMSetRenderTargets(1, &nullRenderTarget, nullptr);
        renderTargetView_.Reset();

        HRESULT hr = swapChain_->ResizeBuffers(0, clientWidth, clientHeight, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LogHResult(L"IDXGISwapChain::ResizeBuffers(SourceReader)", hr);
            return hr;
        }

        swapChainWidth_ = clientWidth;
        swapChainHeight_ = clientHeight;
        Log::Write(L"SourceReader D3D11 swap chain resized: %ux%u", clientWidth, clientHeight);
    }

    if (!renderTargetView_) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        RETURN_IF_FAILED_LOG(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), L"IDXGISwapChain::GetBuffer(SourceReader)");
        RETURN_IF_FAILED_LOG(
            d3dDevice_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView_),
            L"ID3D11Device::CreateRenderTargetView(SourceReader)");
    }

    return S_OK;
}

HRESULT SourceReaderD3D11Player::EnsureFrameTexture(UINT32 frameWidth, UINT32 frameHeight)
{
    if (frameWidth == 0 || frameHeight == 0) {
        return E_INVALIDARG;
    }

    if (frameTexture_ && frameTextureView_ && textureWidth_ == frameWidth && textureHeight_ == frameHeight) {
        return S_OK;
    }

    frameTextureView_.Reset();
    frameTexture_.Reset();

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = frameWidth;
    textureDesc.Height = frameHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    RETURN_IF_FAILED_LOG(
        d3dDevice_->CreateTexture2D(&textureDesc, nullptr, &frameTexture_),
        L"ID3D11Device::CreateTexture2D(SourceReader frame)");
    RETURN_IF_FAILED_LOG(
        d3dDevice_->CreateShaderResourceView(frameTexture_.Get(), nullptr, &frameTextureView_),
        L"ID3D11Device::CreateShaderResourceView(SourceReader frame)");

    textureWidth_ = frameWidth;
    textureHeight_ = frameHeight;
    Log::Write(L"SourceReader D3D11 RGB32 upload texture created: %ux%u", frameWidth, frameHeight);
    return S_OK;
}

HRESULT SourceReaderD3D11Player::RenderRgb32Sample(IMFSample* sample, UINT32 frameWidth, UINT32 frameHeight, DWORD& byteCount)
{
    byteCount = 0;
    RETURN_IF_FAILED_LOG(EnsureRenderResources(frameWidth, frameHeight), L"SourceReaderD3D11Player::EnsureRenderResources");

    if (!swapChain_ || !renderTargetView_) {
        return S_OK;
    }

    RETURN_IF_FAILED_LOG(UploadRgb32Sample(sample, frameWidth, frameHeight, byteCount), L"SourceReaderD3D11Player::UploadRgb32Sample");
    RETURN_IF_FAILED_LOG(DrawFrame(), L"SourceReaderD3D11Player::DrawFrame");
    return S_OK;
}

HRESULT SourceReaderD3D11Player::UploadRgb32Sample(IMFSample* sample, UINT32 frameWidth, UINT32 frameHeight, DWORD& byteCount)
{
    byteCount = 0;
    if (sample == nullptr || !frameTexture_) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED_LOG(sample->ConvertToContiguousBuffer(&buffer), L"IMFSample::ConvertToContiguousBuffer(video)");

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    HRESULT hr = buffer->Lock(&data, &maxLength, &currentLength);
    if (FAILED(hr)) {
        LogHResult(L"IMFMediaBuffer::Lock(video)", hr);
        return hr;
    }

    const UINT sourcePitch = frameWidth * 4;
    const unsigned long long requiredBytes = static_cast<unsigned long long>(sourcePitch) * frameHeight;
    if (currentLength < requiredBytes) {
        LOG_IF_FAILED(buffer->Unlock(), L"IMFMediaBuffer::Unlock(video after short RGB32 buffer)");
        Log::Write(L"SourceReader RGB32 sample too small: bytes=%u required=%llu", currentLength, requiredBytes);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3dContext_->Map(frameTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        LOG_IF_FAILED(buffer->Unlock(), L"IMFMediaBuffer::Unlock(video after D3D map failure)");
        LogHResult(L"ID3D11DeviceContext::Map(SourceReader frame texture)", hr);
        return hr;
    }

    auto* destination = static_cast<BYTE*>(mapped.pData);
    for (UINT32 y = 0; y < frameHeight; ++y) {
        std::memcpy(
            destination + static_cast<size_t>(mapped.RowPitch) * y,
            data + static_cast<size_t>(sourcePitch) * y,
            sourcePitch);
    }
    d3dContext_->Unmap(frameTexture_.Get(), 0);

    hr = buffer->Unlock();
    if (FAILED(hr)) {
        LogHResult(L"IMFMediaBuffer::Unlock(video)", hr);
        return hr;
    }

    byteCount = currentLength;
    return S_OK;
}

HRESULT SourceReaderD3D11Player::DrawFrame()
{
    if (!swapChain_ || !renderTargetView_ || !frameTextureView_ || !vertexShader_ || !pixelShader_ || !sampler_) {
        return S_OK;
    }

    const FLOAT clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    d3dContext_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), nullptr);
    d3dContext_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<FLOAT>(swapChainWidth_);
    viewport.Height = static_cast<FLOAT>(swapChainHeight_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    d3dContext_->RSSetViewports(1, &viewport);

    d3dContext_->IASetInputLayout(nullptr);
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShaderResources(0, 1, frameTextureView_.GetAddressOf());
    d3dContext_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    d3dContext_->Draw(3, 0);

    ID3D11ShaderResourceView* nullShaderResource = nullptr;
    d3dContext_->PSSetShaderResources(0, 1, &nullShaderResource);

    HRESULT hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) {
        LogHResult(L"IDXGISwapChain::Present(SourceReader)", hr);
        return hr;
    }

    return S_OK;
}

void SourceReaderD3D11Player::ResetRenderResources()
{
    if (d3dContext_) {
        ID3D11ShaderResourceView* nullShaderResource = nullptr;
        ID3D11RenderTargetView* nullRenderTarget = nullptr;
        d3dContext_->PSSetShaderResources(0, 1, &nullShaderResource);
        d3dContext_->OMSetRenderTargets(1, &nullRenderTarget, nullptr);
        d3dContext_->ClearState();
        d3dContext_->Flush();
    }

    sampler_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    frameTextureView_.Reset();
    frameTexture_.Reset();
    renderTargetView_.Reset();
    swapChain_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    swapChainWidth_ = 0;
    swapChainHeight_ = 0;
    textureWidth_ = 0;
    textureHeight_ = 0;
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

        hr = ConfigureRgb32Output(sourceReader.Get(), frameWidth, frameHeight);
        if (FAILED(hr)) {
            PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: configuring RGB32 decoder output");
            break;
        }

        width_.store(frameWidth);
        height_.store(frameHeight);
        Log::Write(L"SourceReader D3D11 RGB32 swap-chain renderer started: %ux%u", frameWidth, frameHeight);

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

            DWORD sampleByteCount = 0;
            hr = RenderRgb32Sample(sample.Get(), frameWidth, frameHeight, sampleByteCount);
            if (FAILED(hr)) {
                PaintDiagnosticPatternOnVideoAndParent(hwndVideo_, L"SourceReader failed: rendering decoded RGB32 frame");
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

    Log::Write(L"D3D11 device created for SourceReader renderer. Feature level=0x%04X", static_cast<unsigned int>(createdLevel));
    return S_OK;
}
