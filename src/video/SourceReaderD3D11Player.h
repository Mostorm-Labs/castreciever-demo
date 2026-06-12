#pragma once

#include "video/IVideoPlayer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <thread>

struct IMFSample;

class SourceReaderD3D11Player final : public IVideoPlayer {
public:
    SourceReaderD3D11Player();
    ~SourceReaderD3D11Player() override;

    HRESULT Start(HWND hwndVideo, const VideoStartOptions& options) override;
    void Stop() override;
    void Resize(UINT width, UINT height) override;

private:
    HRESULT InitializeD3D11();
    HRESULT EnsureRenderResources(UINT32 frameWidth, UINT32 frameHeight);
    HRESULT EnsureShaders();
    HRESULT EnsureSwapChain(UINT clientWidth, UINT clientHeight);
    HRESULT EnsureFrameTexture(UINT32 frameWidth, UINT32 frameHeight);
    HRESULT RenderRgb32Sample(IMFSample* sample, UINT32 frameWidth, UINT32 frameHeight, DWORD& byteCount);
    HRESULT UploadRgb32Sample(IMFSample* sample, UINT32 frameWidth, UINT32 frameHeight, DWORD& byteCount);
    HRESULT DrawFrame();
    void ResetRenderResources();
    void WorkerThread(VideoStartOptions options);

    HWND hwndVideo_ = nullptr;
    std::thread worker_;
    std::atomic_bool running_{ false };
    std::atomic_uint width_{ 0 };
    std::atomic_uint height_{ 0 };
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frameTextureView_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    UINT swapChainWidth_ = 0;
    UINT swapChainHeight_ = 0;
    UINT textureWidth_ = 0;
    UINT textureHeight_ = 0;
};
