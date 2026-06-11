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

    HRESULT Start(HWND hwndVideo, const VideoStartOptions& options) override;
    void Stop() override;
    void Resize(UINT width, UINT height) override;

private:
    class CaptureEngineCallback;

    HRESULT ConfigureVideoMediaType(bool preferH264);
    HRESULT ConfigurePreviewSink(PreviewSinkMode mode);
    HRESULT CreatePreviewMediaType(IMFMediaType** mediaType);
    void LogCurrentVideoTypes();

    HWND hwndVideo_ = nullptr;
    Microsoft::WRL::ComPtr<IMFCaptureEngine> captureEngine_;
    Microsoft::WRL::ComPtr<IMFCapturePreviewSink> previewSink_;
    Microsoft::WRL::ComPtr<IMFCaptureEngineOnEventCallback> callback_;
    DWORD previewSourceStreamIndex_ = 0;
    bool hasPreviewSourceStreamIndex_ = false;
    bool started_ = false;
};
