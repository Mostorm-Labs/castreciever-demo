#include "ui/OverlayControls.h"

#include "HResult.h"

#include <algorithm>

namespace {

HWND CreateButton(HWND parent, HINSTANCE instance, int id, const wchar_t* text)
{
    return CreateWindowExW(
        0,
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        0,
        0,
        0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance,
        nullptr);
}

} // namespace

HRESULT OverlayControls::Create(HWND parent, HINSTANCE instance)
{
    parent_ = parent;

    muteButton_ = CreateButton(parent, instance, ID_BTN_MUTE, L"Mute");
    if (muteButton_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateButton(Mute)", hr);
        return hr;
    }

    stopButton_ = CreateButton(parent, instance, ID_BTN_STOP, L"Stop");
    if (stopButton_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateButton(Stop)", hr);
        return hr;
    }

    maxRestoreButton_ = CreateButton(parent, instance, ID_BTN_MAX_RESTORE, L"Maximize");
    if (maxRestoreButton_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateButton(Maximize)", hr);
        return hr;
    }

    return S_OK;
}

void OverlayControls::Layout(const RECT& client)
{
    if (muteButton_ == nullptr || stopButton_ == nullptr || maxRestoreButton_ == nullptr) {
        return;
    }

    constexpr int kButtonWidth = 112;
    constexpr int kButtonHeight = 34;
    constexpr int kGap = 10;
    constexpr int kMargin = 18;

    const int totalWidth = kButtonWidth * 3 + kGap * 2;
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    const int startX = std::max(kMargin, clientWidth - totalWidth - kMargin);
    const int y = std::max(kMargin, clientHeight - kButtonHeight - kMargin);

    MoveWindow(muteButton_, startX, y, kButtonWidth, kButtonHeight, TRUE);
    MoveWindow(stopButton_, startX + kButtonWidth + kGap, y, kButtonWidth, kButtonHeight, TRUE);
    MoveWindow(maxRestoreButton_, startX + (kButtonWidth + kGap) * 2, y, kButtonWidth, kButtonHeight, TRUE);

    SetWindowPos(muteButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(stopButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(maxRestoreButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void OverlayControls::UpdateMuted(bool muted)
{
    if (muteButton_ != nullptr) {
        SetWindowTextW(muteButton_, muted ? L"Unmute" : L"Mute");
    }
}

void OverlayControls::UpdateMaximized(bool maximized)
{
    if (maxRestoreButton_ != nullptr) {
        SetWindowTextW(maxRestoreButton_, maximized ? L"Restore" : L"Maximize");
    }
}
