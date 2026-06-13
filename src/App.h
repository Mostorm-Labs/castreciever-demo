#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "video/IVideoPlayer.h"

class AirPlayDiscoveryService;
class HidMediaExperimentalAdapter;
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
    bool hidRuntimeTransportEnabled = false;
    uint16_t hidVendorId = 0;
    uint16_t hidProductId = 0;
    std::string hidSerialNumber;
    uint8_t hidReportId = 0;
    size_t hidInputReportSize = 64;
    size_t hidOutputReportSize = 64;
    size_t hidMaxReportsPerPoll = 16;
    uint32_t hidPollIntervalMs = 1;
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
    HRESULT StartHidExperimentalSource(const AppOptions& options);

    std::unique_ptr<MediaCore> mediaCore_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<AirPlayDiscoveryService> airPlayDiscovery_;
    std::unique_ptr<HidMediaExperimentalAdapter> hidMediaAdapter_;
    std::unique_ptr<IVideoPlayer> videoPlayer_;
    std::unique_ptr<IAudioPlayer> audioPlayer_;
    std::unique_ptr<MainWindow> mainWindow_;
    bool initialized_ = false;
};
