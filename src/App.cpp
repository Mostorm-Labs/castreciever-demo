#include "App.h"

#include "HResult.h"
#include "MainWindow.h"
#include "audio/IAudioPlayer.h"
#include "audio/WasapiPcmRelay.h"
#include "video/IVideoPlayer.h"
#include "video/MfCapturePreviewPlayer.h"
#include "video/SourceReaderD3D11Player.h"

#include <memory>

App::App() = default;

App::~App()
{
    Shutdown();
}

HRESULT App::Initialize(HINSTANCE instance, int showCommand, const AppOptions& options)
{
    if (initialized_) {
        return S_OK;
    }

    if (options.videoBackend == VideoBackend::SourceReader) {
        videoPlayer_ = std::make_unique<SourceReaderD3D11Player>();
    } else {
        videoPlayer_ = std::make_unique<MfCapturePreviewPlayer>();
    }
    audioPlayer_ = std::make_unique<WasapiPcmRelay>();
    mainWindow_ = std::make_unique<MainWindow>();

    RETURN_IF_FAILED_LOG(
        mainWindow_->Create(instance, showCommand, videoPlayer_.get(), audioPlayer_.get()),
        L"MainWindow::Create");

    VideoStartOptions videoOptions;
    videoOptions.deviceMatch = options.uvcMatch;
    videoOptions.preferH264 = options.preferH264;
    videoOptions.previewSinkMode = options.previewSinkMode;

    HRESULT hr = videoPlayer_->Start(mainWindow_->VideoHwnd(), videoOptions);
    if (FAILED(hr)) {
        LogHResult(L"IVideoPlayer::Start", hr);
        mainWindow_->Destroy();
        return hr;
    }

    hr = audioPlayer_->Start(options.uacMatch);
    if (FAILED(hr)) {
        LogHResult(L"IAudioPlayer::Start", hr);
        videoPlayer_->Stop();
        mainWindow_->Destroy();
        return hr;
    }

    initialized_ = true;
    return S_OK;
}

int App::Run()
{
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

void App::Shutdown()
{
    if (audioPlayer_) {
        audioPlayer_->Stop();
    }

    if (videoPlayer_) {
        videoPlayer_->Stop();
    }

    if (mainWindow_) {
        mainWindow_->Destroy();
    }

    initialized_ = false;
}
