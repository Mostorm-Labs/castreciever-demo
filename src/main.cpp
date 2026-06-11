#include "App.h"
#include "HResult.h"
#include "Log.h"

#include <mfapi.h>
#include <shellapi.h>

#include <string>

namespace {

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
    Log::Write(L"Starting UsbCastReceiver. UVC match='%s', UAC match='%s'",
        options.uvcMatch.c_str(),
        options.uacMatch.c_str());

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
