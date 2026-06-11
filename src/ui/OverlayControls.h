#pragma once

#include <windows.h>

constexpr int ID_BTN_MUTE = 1001;
constexpr int ID_BTN_STOP = 1002;
constexpr int ID_BTN_MAX_RESTORE = 1003;

class OverlayControls {
public:
    HRESULT Create(HWND parent, HINSTANCE instance);
    void Layout(const RECT& client);
    void UpdateMuted(bool muted);
    void UpdateMaximized(bool maximized);

private:
    HWND parent_ = nullptr;
    HWND muteButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND maxRestoreButton_ = nullptr;
};
