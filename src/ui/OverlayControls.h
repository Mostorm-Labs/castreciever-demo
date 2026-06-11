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
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC dc);
    int HitTest(POINT point) const;
    void Invalidate();

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    bool muted_ = false;
    bool maximized_ = false;
    int hoverCommandId_ = 0;
    int pressedCommandId_ = 0;
    bool trackingMouse_ = false;
};
