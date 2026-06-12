#pragma once

#include "video/IVideoPlayer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct IMFDXGIDeviceManager;
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
    HRESULT InitializeDxgiDeviceManager();
    HRESULT EnsureRenderResources(UINT32 frameWidth, UINT32 frameHeight, UINT32 frameRateNumerator, UINT32 frameRateDenominator);
    HRESULT EnsureSwapChain(UINT clientWidth, UINT clientHeight);
    HRESULT EnsureVideoProcessor(UINT32 frameWidth, UINT32 frameHeight, UINT32 frameRateNumerator, UINT32 frameRateDenominator);
    HRESULT RenderNv12Sample(IMFSample* sample, UINT32 frameWidth, UINT32 frameHeight, UINT32 frameRateNumerator, UINT32 frameRateDenominator, DWORD& byteCount);
    HRESULT GetNv12InputView(
        ID3D11Texture2D* texture,
        UINT subresourceIndex,
        ID3D11VideoProcessorInputView** inputView);
    HRESULT DrawNv12Frame(ID3D11VideoProcessorInputView* inputView, UINT32 frameWidth, UINT32 frameHeight);
    void ResetRenderResources();
    void WorkerThread(VideoStartOptions options);

    struct CachedInputView {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT subresourceIndex = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    };

    HWND hwndVideo_ = nullptr;
    std::thread worker_;
    std::atomic_bool running_{ false };
    std::atomic_uint width_{ 0 };
    std::atomic_uint height_{ 0 };
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    UINT dxgiDeviceManagerResetToken_ = 0;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> swapChainBackBuffer_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> videoOutputView_;
    std::vector<CachedInputView> inputViewCache_;
    UINT swapChainWidth_ = 0;
    UINT swapChainHeight_ = 0;
    UINT videoProcessorInputWidth_ = 0;
    UINT videoProcessorInputHeight_ = 0;
    UINT videoProcessorInputFrameRateNumerator_ = 0;
    UINT videoProcessorInputFrameRateDenominator_ = 0;
    UINT videoProcessorOutputWidth_ = 0;
    UINT videoProcessorOutputHeight_ = 0;
};
