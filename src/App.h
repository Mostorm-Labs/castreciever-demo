#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "video/IVideoPlayer.h"

class AirPlayDiscoveryService;
class MainWindow;
class MediaCore;
class SessionManager;
class IVideoPlayer;
class IAudioPlayer;

enum class SourceMode {
    Auto,
    UsbOnly,
    AirPlayOnly,
    HidExperimental,
};

struct AppOptions {
    std::wstring uvcMatch;
    std::wstring uacMatch;
    std::wstring airplayName = L"UsbCastReceiver";
    VideoBackend videoBackend = VideoBackend::SourceReader;
    SourceMode sourceMode = SourceMode::Auto;
    bool preferH264 = true;
    bool airplayPin = false;
    bool noAirPlay = false;
    bool noUsb = false;
    UINT32 targetVideoFps = 0;
    PreviewSinkMode previewSinkMode = PreviewSinkMode::Default;
};

class App {
public:
    App();
    ~App();

    HRESULT Initialize(HINSTANCE instance, int showCommand, const AppOptions& options);
    int Run();
    void Shutdown();

private:
    HRESULT StartAirPlayDiscovery(const AppOptions& options);

    std::unique_ptr<MediaCore> mediaCore_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<AirPlayDiscoveryService> airPlayDiscovery_;
    std::unique_ptr<IVideoPlayer> videoPlayer_;
    std::unique_ptr<IAudioPlayer> audioPlayer_;
    std::unique_ptr<MainWindow> mainWindow_;
    bool initialized_ = false;
};
