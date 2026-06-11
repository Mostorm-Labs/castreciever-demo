#pragma once

#include "video/IVideoPlayer.h"

#include <mfcaptureengine.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

class MfCapturePreviewPlayer final : public IVideoPlayer {
public:
    MfCapturePreviewPlayer();
    ~MfCapturePreviewPlayer() override;

    HRESULT Start(HWND hwndVideo, const std::wstring& deviceMatch) override;
    void Stop() override;
    void Resize(UINT width, UINT height) override;

private:
    class CaptureEngineCallback;

    HRESULT ConfigureH264IfAvailable();
    void LogCurrentVideoTypes();

    HWND hwndVideo_ = nullptr;
    Microsoft::WRL::ComPtr<IMFCaptureEngine> captureEngine_;
    Microsoft::WRL::ComPtr<IMFCapturePreviewSink> previewSink_;
    Microsoft::WRL::ComPtr<IMFCaptureEngineOnEventCallback> callback_;
    bool started_ = false;
};
