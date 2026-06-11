#pragma once

#include <windows.h>

#include <memory>
#include <string>

class MainWindow;
class IVideoPlayer;
class IAudioPlayer;

struct AppOptions {
    std::wstring uvcMatch;
    std::wstring uacMatch;
};

class App {
public:
    App();
    ~App();

    HRESULT Initialize(HINSTANCE instance, int showCommand, const AppOptions& options);
    int Run();
    void Shutdown();

private:
    std::unique_ptr<IVideoPlayer> videoPlayer_;
    std::unique_ptr<IAudioPlayer> audioPlayer_;
    std::unique_ptr<MainWindow> mainWindow_;
    bool initialized_ = false;
};
