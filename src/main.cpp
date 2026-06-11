#include "App.h"
#include "HResult.h"
#include "Log.h"

#include <mfapi.h>
#include <shellapi.h>

#include <string>

namespace {

#ifndef USB_CAST_RECEIVER_GIT_SHA
#define USB_CAST_RECEIVER_GIT_SHA L"unknown"
#endif

#ifndef USB_CAST_RECEIVER_BUILD_TIME_UTC
#define USB_CAST_RECEIVER_BUILD_TIME_UTC L"unknown"
#endif

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
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHResult(L"CoInitializeEx", hr);
        return static_cast<int>(hr);
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LogHResult(L"MFStartup", hr);
        CoUninitialize();
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

    Log::Write(L"Starting UsbCastReceiver. UVC match='%s', UAC match='%s', video-backend='%s', video-format='%s', preview-sink='%d'",
        options.uvcMatch.c_str(),
        options.uacMatch.c_str(),
        backendName,
        options.preferH264 ? L"h264" : L"auto",
        static_cast<int>(options.previewSinkMode));

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
    return exitCode;
}
