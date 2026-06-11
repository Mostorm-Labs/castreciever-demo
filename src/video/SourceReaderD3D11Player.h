#pragma once

#include "video/IVideoPlayer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <thread>

class SourceReaderD3D11Player final : public IVideoPlayer {
public:
    SourceReaderD3D11Player();
    ~SourceReaderD3D11Player() override;

    HRESULT Start(HWND hwndVideo, const VideoStartOptions& options) override;
    void Stop() override;
    void Resize(UINT width, UINT height) override;

private:
    HRESULT InitializeD3D11();
    void WorkerThread(VideoStartOptions options);

    HWND hwndVideo_ = nullptr;
    std::thread worker_;
    std::atomic_bool running_{ false };
    std::atomic_uint width_{ 0 };
    std::atomic_uint height_{ 0 };
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
};
