#include "App.h"

#include "HResult.h"
#include "MainWindow.h"
#include "airplay/AirPlayDeviceId.h"
#include "airplay/AirPlayDiscoveryService.h"
#include "airplay/AirPlayFeatures.h"
#include "audio/IAudioPlayer.h"
#include "audio/WasapiPcmRelay.h"
#include "core/MediaCore.h"
#include "hid/HidMediaExperimentalAdapter.h"
#include "source/SessionManager.h"
#include "video/IVideoPlayer.h"
#include "video/MfCapturePreviewPlayer.h"
#include "video/SelfTestVideoPlayer.h"
#include "video/SourceReaderD3D11Player.h"

#include <memory>

namespace {

bool WantsUsb(const AppOptions& options)
{
    if (options.noUsb) {
        return false;
    }
    return options.sourceMode == SourceMode::Auto || options.sourceMode == SourceMode::UsbOnly;
}

bool WantsAirPlay(const AppOptions& options)
{
    if (options.noAirPlay) {
        return false;
    }
    return options.sourceMode == SourceMode::Auto || options.sourceMode == SourceMode::AirPlayOnly;
}

bool WantsHidExperimental(const AppOptions& options)
{
    return options.sourceMode == SourceMode::HidExperimental;
}

} // namespace

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

    if (options.videoBackend == VideoBackend::SelfTest) {
        videoPlayer_ = std::make_unique<SelfTestVideoPlayer>();
    } else if (options.videoBackend == VideoBackend::SourceReader) {
        videoPlayer_ = std::make_unique<SourceReaderD3D11Player>();
    } else {
        videoPlayer_ = std::make_unique<MfCapturePreviewPlayer>();
    }
    audioPlayer_ = std::make_unique<WasapiPcmRelay>();
    mediaCore_ = std::make_unique<MediaCore>();
    sessionManager_ = std::make_unique<SessionManager>();
    airPlayDiscovery_ = std::make_unique<AirPlayDiscoveryService>();
    hidMediaAdapter_ = std::make_unique<HidMediaExperimentalAdapter>();
    mainWindow_ = std::make_unique<MainWindow>();

    RETURN_IF_FAILED_LOG(
        mainWindow_->Create(instance, showCommand, videoPlayer_.get(), audioPlayer_.get()),
        L"MainWindow::Create");

    if (WantsAirPlay(options)) {
        HRESULT discoveryHr = StartAirPlayDiscovery(options);
        if (FAILED(discoveryHr)) {
            LogHResult(L"App::StartAirPlayDiscovery", discoveryHr);
            Log::Write(L"AirPlay discovery disabled; continuing with the remaining enabled sources.");
        }
    } else {
        Log::Write(L"AirPlay disabled by source mode or --no-airplay.");
    }

    VideoStartOptions videoOptions;
    videoOptions.deviceMatch = options.uvcMatch;
    videoOptions.preferH264 = options.preferH264;
    videoOptions.targetVideoFps = options.targetVideoFps;
    videoOptions.previewSinkMode = options.previewSinkMode;

    HRESULT hr = S_OK;
    bool usbVideoStarted = false;
    if (WantsUsb(options)) {
        sessionManager_->TryBegin(MediaSourceKind::Usb);
        hr = videoPlayer_->Start(mainWindow_->VideoHwnd(), videoOptions);
        if (FAILED(hr)) {
            LogHResult(L"IVideoPlayer::Start", hr);
            sessionManager_->End(MediaSourceKind::Usb);
            if (options.sourceMode == SourceMode::UsbOnly || !WantsAirPlay(options)) {
                mainWindow_->Destroy();
                return hr;
            }
            Log::Write(L"USB video startup failed in auto mode; keeping receiver window alive for AirPlay.");
        } else {
            usbVideoStarted = true;
        }
    } else if (WantsHidExperimental(options)) {
        hr = StartHidExperimentalSource(options);
        if (FAILED(hr)) {
            mainWindow_->Destroy();
            return hr;
        }
    } else {
        Log::Write(L"USB disabled by source mode or --no-usb.");
    }

    if (!usbVideoStarted) {
        Log::Write(L"USB audio startup skipped because USB video is not active.");
    } else if (options.videoBackend == VideoBackend::SelfTest) {
        Log::Write(L"Self-test video backend selected; skipping audio startup.");
    } else {
        hr = audioPlayer_->Start(options.uacMatch);
        if (FAILED(hr)) {
            LogHResult(L"IAudioPlayer::Start", hr);
            Log::Write(L"Audio startup failed; continuing video-only so the receiver window stays available for diagnostics.");
        }
    }

    initialized_ = true;
    return S_OK;
}

HRESULT App::StartHidExperimentalSource(const AppOptions& options)
{
    if (!sessionManager_->TryBegin(MediaSourceKind::HidExperimental, L"AXTP HID")) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    if (!mediaCore_->StartSession(MediaSourceKind::HidExperimental)) {
        sessionManager_->End(MediaSourceKind::HidExperimental);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    SourceStartContext sourceContext;
    sourceContext.mediaSink = mediaCore_.get();
    sourceContext.videoHwnd = mainWindow_ ? mainWindow_->VideoHwnd() : nullptr;

    HidRuntimeTransportConfig transportConfig;
    transportConfig.enabled = options.hidRuntimeTransportEnabled;
    transportConfig.vendorId = options.hidVendorId;
    transportConfig.productId = options.hidProductId;
    transportConfig.serialNumber = options.hidSerialNumber;
    transportConfig.reportId = options.hidReportId;
    transportConfig.inputReportSize = options.hidInputReportSize;
    transportConfig.outputReportSize = options.hidOutputReportSize;
    transportConfig.maxReportsPerPoll = options.hidMaxReportsPerPoll;
    transportConfig.pollIntervalMs = options.hidPollIntervalMs;
    if (transportConfig.enabled && (transportConfig.vendorId == 0 || transportConfig.productId == 0)) {
        Log::Write(L"HID runtime transport requires both --hid-vid and --hid-pid.");
        mediaCore_->EndSession(MediaSourceKind::HidExperimental);
        sessionManager_->End(MediaSourceKind::HidExperimental);
        return E_INVALIDARG;
    }
    hidMediaAdapter_->SetRuntimeTransportConfig(transportConfig);

    const HRESULT hr = hidMediaAdapter_->Start(sourceContext);
    if (FAILED(hr)) {
        mediaCore_->EndSession(MediaSourceKind::HidExperimental);
        sessionManager_->End(MediaSourceKind::HidExperimental);
        LogHResult(L"HidMediaExperimentalAdapter::Start", hr);
        return hr;
    }

    Log::Write(L"HID experimental source mode selected; AXTP Standard Frame/STREAM parser is active.");
    return S_OK;
}

HRESULT App::StartAirPlayDiscovery(const AppOptions& options)
{
    AirPlayDeviceIdentity identity = ResolveAirPlayDeviceIdentity();
    const std::string deviceId = FormatAirPlayDeviceId(identity.deviceId);
    Log::Write(
        L"AirPlay device id resolved from %s: %S",
        identity.fromNetworkAdapter ? L"network adapter" : L"persistent random fallback",
        deviceId.c_str());

    AirPlayDiscoveryOptions discoveryOptions;
    discoveryOptions.name = options.airplayName.empty() ? L"UsbCastReceiver" : options.airplayName;
    discoveryOptions.deviceId = identity.deviceId;
    discoveryOptions.features = AirPlayFeatureSet::DefaultMirrorH264V1(true);
    discoveryOptions.pinMode = options.airplayPin ? AirPlayPinMode::OnscreenPin : AirPlayPinMode::None;
    discoveryOptions.publicKeyHex = ResolvePersistentDiscoveryPublicKeyHex();

    HRESULT hr = airPlayDiscovery_->Start(discoveryOptions);
    if (SUCCEEDED(hr)) {
        Log::Write(L"AirPlay DNS-SD discovery is active. RAOP protocol server migration is not enabled in this build yet.");
    }
    return hr;
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
    if (hidMediaAdapter_) {
        hidMediaAdapter_->Stop();
    }

    if (airPlayDiscovery_) {
        airPlayDiscovery_->Stop();
    }

    if (audioPlayer_) {
        audioPlayer_->Stop();
    }

    if (videoPlayer_) {
        videoPlayer_->Stop();
    }

    if (mainWindow_) {
        mainWindow_->Destroy();
    }

    if (sessionManager_) {
        sessionManager_->ForceIdle();
    }
    if (mediaCore_) {
        mediaCore_->EndSession(MediaSourceKind::Unknown);
    }

    initialized_ = false;
}
