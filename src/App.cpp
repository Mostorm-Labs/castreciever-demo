#include "App.h"

#include "HResult.h"
#include "MainWindow.h"
#include "audio/IAudioPlayer.h"
#include "audio/WasapiPcmRelay.h"
#include "video/IVideoPlayer.h"
#include "video/MfCapturePreviewPlayer.h"
#include "video/SelfTestVideoPlayer.h"
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
        Log::Write(L"App::Initialize skipped because app is already initialized.");
        return S_OK;
    }

    Log::Checkpoint(L"App::Initialize selecting video backend");
    if (options.videoBackend == VideoBackend::SelfTest) {
        videoPlayer_ = std::make_unique<SelfTestVideoPlayer>();
        Log::Write(L"App created SelfTestVideoPlayer.");
    } else if (options.videoBackend == VideoBackend::SourceReader) {
        videoPlayer_ = std::make_unique<SourceReaderD3D11Player>();
        Log::Write(L"App created SourceReaderD3D11Player.");
    } else {
        videoPlayer_ = std::make_unique<MfCapturePreviewPlayer>();
        Log::Write(L"App created MfCapturePreviewPlayer.");
    }
    audioPlayer_ = std::make_unique<WasapiPcmRelay>();
    mainWindow_ = std::make_unique<MainWindow>();
    Log::Write(L"App created audio player and main window objects.");

    Log::Checkpoint(L"MainWindow::Create");
    RETURN_IF_FAILED_LOG(
        mainWindow_->Create(instance, showCommand, videoPlayer_.get(), audioPlayer_.get()),
        L"MainWindow::Create");
    Log::Write(L"MainWindow created. hwnd=0x%p videoHwnd=0x%p", mainWindow_->Hwnd(), mainWindow_->VideoHwnd());

    VideoStartOptions videoOptions;
    videoOptions.deviceMatch = options.uvcMatch;
    videoOptions.preferH264 = options.preferH264;
    videoOptions.targetVideoFps = options.targetVideoFps;
    videoOptions.previewSinkMode = options.previewSinkMode;

    Log::Checkpoint(L"IVideoPlayer::Start hwndVideo=0x%p", mainWindow_->VideoHwnd());
    HRESULT hr = videoPlayer_->Start(mainWindow_->VideoHwnd(), videoOptions);
    if (FAILED(hr)) {
        LogHResult(L"IVideoPlayer::Start", hr);
        mainWindow_->Destroy();
        return hr;
    }
    Log::Write(L"IVideoPlayer::Start returned successfully.");

    if (options.videoBackend == VideoBackend::SelfTest) {
        Log::Write(L"Self-test video backend selected; skipping audio startup.");
    } else {
        Log::Checkpoint(L"IAudioPlayer::Start");
        hr = audioPlayer_->Start(options.uacMatch);
        if (FAILED(hr)) {
            LogHResult(L"IAudioPlayer::Start", hr);
            Log::Write(L"Audio startup failed; continuing video-only so the receiver window stays available for diagnostics.");
        } else {
            Log::Write(L"IAudioPlayer::Start returned successfully.");
        }
    }

    Log::Checkpoint(L"App::Initialize complete");
    initialized_ = true;
    return S_OK;
}

int App::Run()
{
    Log::Write(L"Entering Win32 message loop.");
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

void App::Shutdown()
{
    Log::Checkpoint(L"App::Shutdown stopping media");
    if (audioPlayer_) {
        Log::Write(L"Stopping audio player.");
        audioPlayer_->Stop();
    }

    if (videoPlayer_) {
        Log::Write(L"Stopping video player.");
        videoPlayer_->Stop();
    }

    if (mainWindow_) {
        Log::Write(L"Destroying main window.");
        mainWindow_->Destroy();
    }

    Log::Write(L"App::Shutdown complete.");
    initialized_ = false;
}
