#pragma once

#include "video/IVideoPlayer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class SourceReaderD3D11Player final : public IVideoPlayer {
public:
    SourceReaderD3D11Player();
    ~SourceReaderD3D11Player() override;

    HRESULT Start(HWND hwndVideo, const std::wstring& deviceMatch) override;
    void Stop() override;
    void Resize(UINT width, UINT height) override;

private:
    HRESULT InitializeD3D11();

    HWND hwndVideo_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
};
