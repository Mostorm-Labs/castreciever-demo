#pragma once

#include "ui/OverlayControls.h"

#include <windows.h>

class IAudioPlayer;
class IVideoPlayer;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    HRESULT Create(HINSTANCE instance, int showCommand, IVideoPlayer* videoPlayer, IAudioPlayer* audioPlayer);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    HWND VideoHwnd() const { return videoHwnd_; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HRESULT RegisterWindowClass();
    HRESULT OnCreate();
    void OnSize(UINT width, UINT height);
    void OnCommand(int commandId);
    void StopMedia();
    void ToggleMute();
    void ToggleMaximizeRestore();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND videoHwnd_ = nullptr;
    IVideoPlayer* videoPlayer_ = nullptr;
    IAudioPlayer* audioPlayer_ = nullptr;
    OverlayControls controls_;
    bool stopping_ = false;
};
