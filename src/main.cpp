#include "App.h"
#include "HResult.h"
#include "Log.h"

#include <mfapi.h>
#include <shellapi.h>

#include <cstdlib>
#include <exception>
#include <string>

namespace {

#ifndef USB_CAST_RECEIVER_GIT_SHA
#define USB_CAST_RECEIVER_GIT_SHA L"unknown"
#endif

#ifndef USB_CAST_RECEIVER_BUILD_TIME_UTC
#define USB_CAST_RECEIVER_BUILD_TIME_UTC L"unknown"
#endif

UINT32 ParseVideoFpsValue(const std::wstring& value)
{
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != L'\0' || parsed == 0 || parsed > 240) {
        Log::Write(L"Invalid --video-fps value ignored: %s", value.c_str());
        return 0;
    }

    return static_cast<UINT32>(parsed);
}

bool ParseUnsignedValue(const std::wstring& value, unsigned long maxValue, unsigned long& parsed)
{
    wchar_t* end = nullptr;
    parsed = std::wcstoul(value.c_str(), &end, 0);
    return !value.empty() && end != value.c_str() && *end == L'\0' && parsed <= maxValue;
}

std::string NarrowAscii(const std::wstring& value)
{
    std::string result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        result.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
    }
    return result;
}

const wchar_t* SourceModeName(SourceMode sourceMode)
{
    switch (sourceMode) {
    case SourceMode::Auto:
        return L"auto";
    case SourceMode::UsbOnly:
        return L"usb-only";
    case SourceMode::AirPlayOnly:
        return L"airplay-only";
    case SourceMode::HidExperimental:
        return L"hid-experimental";
    default:
        return L"unknown";
    }
}

void EnableDpiAwareness()
{
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        Log::Write(L"DPI awareness enabled: Per-Monitor V2.");
        return;
    }

    DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED) {
        Log::Write(L"DPI awareness was already set before startup.");
        return;
    }

    LogHResult(L"SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)", HRESULT_FROM_WIN32(error));
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
        Log::Write(L"DPI awareness enabled: Per-Monitor.");
        return;
    }

    error = GetLastError();
    if (error == ERROR_ACCESS_DENIED) {
        Log::Write(L"DPI awareness was already set before fallback.");
        return;
    }

    LogHResult(L"SetProcessDpiAwarenessContext(PER_MONITOR_AWARE)", HRESULT_FROM_WIN32(error));
}

AppOptions ParseCommandLine()
{
    AppOptions options;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        LogHResult(L"CommandLineToArgvW", HResultFromLastError());
        return options;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--uvc-match" && i + 1 < argc) {
            options.uvcMatch = argv[++i];
        } else if (arg == L"--uac-match" && i + 1 < argc) {
            options.uacMatch = argv[++i];
        } else if (arg == L"--source" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"auto") {
                options.sourceMode = SourceMode::Auto;
            } else if (value == L"usb-only") {
                options.sourceMode = SourceMode::UsbOnly;
            } else if (value == L"airplay-only") {
                options.sourceMode = SourceMode::AirPlayOnly;
            } else if (value == L"hid-experimental") {
                options.sourceMode = SourceMode::HidExperimental;
            } else {
                Log::Write(L"Unknown --source value ignored: %s", value.c_str());
            }
        } else if (arg == L"--airplay-name" && i + 1 < argc) {
            options.airplayName = argv[++i];
        } else if (arg == L"--airplay-pin") {
            options.airplayPin = true;
        } else if (arg == L"--no-airplay") {
            options.noAirPlay = true;
        } else if (arg == L"--no-usb") {
            options.noUsb = true;
        } else if (arg == L"--video-backend" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"capture") {
                options.videoBackend = VideoBackend::CaptureEngine;
            } else if (value == L"source-reader") {
                options.videoBackend = VideoBackend::SourceReader;
            } else if (value == L"self-test") {
                options.videoBackend = VideoBackend::SelfTest;
            } else {
                Log::Write(L"Unknown --video-backend value ignored: %s", value.c_str());
            }
        } else if (arg == L"--video-format" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"auto") {
                options.preferH264 = false;
            } else if (value == L"h264") {
                options.preferH264 = true;
            } else {
                Log::Write(L"Unknown --video-format value ignored: %s", value.c_str());
            }
        } else if (arg == L"--video-fps" && i + 1 < argc) {
            options.targetVideoFps = ParseVideoFpsValue(argv[++i]);
        } else if (arg == L"--hid-vid" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 0xFFFF, parsed)) {
                options.hidVendorId = static_cast<uint16_t>(parsed);
                options.hidRuntimeTransportEnabled = true;
            } else {
                Log::Write(L"Invalid --hid-vid value ignored.");
            }
        } else if (arg == L"--hid-pid" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 0xFFFF, parsed)) {
                options.hidProductId = static_cast<uint16_t>(parsed);
                options.hidRuntimeTransportEnabled = true;
            } else {
                Log::Write(L"Invalid --hid-pid value ignored.");
            }
        } else if (arg == L"--hid-serial" && i + 1 < argc) {
            options.hidSerialNumber = NarrowAscii(argv[++i]);
        } else if (arg == L"--hid-report-id" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 0xFF, parsed)) {
                options.hidReportId = static_cast<uint8_t>(parsed);
            } else {
                Log::Write(L"Invalid --hid-report-id value ignored.");
            }
        } else if (arg == L"--hid-report-size" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 65535, parsed) && parsed >= 2) {
                options.hidInputReportSize = static_cast<size_t>(parsed);
                options.hidOutputReportSize = static_cast<size_t>(parsed);
            } else {
                Log::Write(L"Invalid --hid-report-size value ignored.");
            }
        } else if (arg == L"--hid-input-report-size" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 65535, parsed) && parsed >= 2) {
                options.hidInputReportSize = static_cast<size_t>(parsed);
            } else {
                Log::Write(L"Invalid --hid-input-report-size value ignored.");
            }
        } else if (arg == L"--hid-output-report-size" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 65535, parsed) && parsed >= 2) {
                options.hidOutputReportSize = static_cast<size_t>(parsed);
            } else {
                Log::Write(L"Invalid --hid-output-report-size value ignored.");
            }
        } else if (arg == L"--hid-max-reports-per-poll" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 1024, parsed) && parsed > 0) {
                options.hidMaxReportsPerPoll = static_cast<size_t>(parsed);
            } else {
                Log::Write(L"Invalid --hid-max-reports-per-poll value ignored.");
            }
        } else if (arg == L"--hid-poll-ms" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (ParseUnsignedValue(argv[++i], 1000, parsed) && parsed > 0) {
                options.hidPollIntervalMs = static_cast<uint32_t>(parsed);
            } else {
                Log::Write(L"Invalid --hid-poll-ms value ignored.");
            }
        } else if (arg == L"--preview-sink" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"default") {
                options.previewSinkMode = PreviewSinkMode::Default;
            } else if (value == L"add-stream") {
                options.previewSinkMode = PreviewSinkMode::AutoAddStream;
            } else if (value == L"rgb32") {
                options.previewSinkMode = PreviewSinkMode::Rgb32AddStream;
            } else {
                Log::Write(L"Unknown --preview-sink value ignored: %s", value.c_str());
            }
        } else {
            Log::Write(L"Unknown argument ignored: %s", arg.c_str());
        }
    }

    LocalFree(argv);
    return options;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    Log::Initialize();
    SetUnhandledExceptionFilter(Log::UnhandledExceptionFilter);
    std::set_terminate(Log::TerminateHandler);
    EnableDpiAwareness();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHResult(L"CoInitializeEx", hr);
        Log::Shutdown();
        return static_cast<int>(hr);
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LogHResult(L"MFStartup", hr);
        CoUninitialize();
        Log::Shutdown();
        return static_cast<int>(hr);
    }

    const AppOptions options = ParseCommandLine();
    Log::Write(L"UsbCastReceiver build git='%s' built='%s'", USB_CAST_RECEIVER_GIT_SHA, USB_CAST_RECEIVER_BUILD_TIME_UTC);

    const wchar_t* backendName = L"capture";
    if (options.videoBackend == VideoBackend::SourceReader) {
        backendName = L"source-reader";
    } else if (options.videoBackend == VideoBackend::SelfTest) {
        backendName = L"self-test";
    }

    Log::Write(L"Starting UsbCastReceiver. source='%s', UVC match='%s', UAC match='%s', airplay-name='%s', airplay-pin=%s, no-airplay=%s, no-usb=%s, video-backend='%s', video-format='%s', video-fps=%u, preview-sink='%d', hid-runtime=%s, hid-vid=0x%04X, hid-pid=0x%04X, hid-report-id=%u, hid-input-report=%zu, hid-output-report=%zu, hid-max-reports-per-poll=%zu, hid-poll-ms=%u",
        SourceModeName(options.sourceMode),
        options.uvcMatch.c_str(),
        options.uacMatch.c_str(),
        options.airplayName.c_str(),
        options.airplayPin ? L"true" : L"false",
        options.noAirPlay ? L"true" : L"false",
        options.noUsb ? L"true" : L"false",
        backendName,
        options.preferH264 ? L"h264" : L"auto",
        options.targetVideoFps,
        static_cast<int>(options.previewSinkMode),
        options.hidRuntimeTransportEnabled ? L"true" : L"false",
        options.hidVendorId,
        options.hidProductId,
        options.hidReportId,
        options.hidInputReportSize,
        options.hidOutputReportSize,
        options.hidMaxReportsPerPoll,
        options.hidPollIntervalMs);

    int exitCode = 0;
    {
        App app;
        hr = app.Initialize(instance, showCommand, options);
        if (SUCCEEDED(hr)) {
            exitCode = app.Run();
        } else {
            LogHResult(L"App::Initialize", hr);
            exitCode = static_cast<int>(hr);
        }
        app.Shutdown();
    }

    LOG_IF_FAILED(MFShutdown(), L"MFShutdown");
    CoUninitialize();
    Log::Shutdown();
    return exitCode;
}
