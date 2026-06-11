#include "MainWindow.h"

#include "HResult.h"
#include "Log.h"
#include "audio/IAudioPlayer.h"
#include "video/IVideoPlayer.h"

#include <strsafe.h>

namespace {

constexpr wchar_t kMainWindowClassName[] = L"UsbCastReceiverMainWindow";
constexpr wchar_t kVideoWindowClassName[] = L"UsbCastReceiverVideoWindow";
constexpr wchar_t kSelfTestPaintProperty[] = L"UsbCastReceiverSelfTestPaint";

LRESULT CALLBACK VideoWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        if (GetPropW(hwnd, kSelfTestPaintProperty) != nullptr) {
            RECT client = {};
            GetClientRect(hwnd, &client);
            const COLORREF colors[] = {
                RGB(255, 0, 0),
                RGB(0, 180, 0),
                RGB(0, 80, 255),
                RGB(255, 220, 0),
            };
            const int width = client.right - client.left;
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(colors)); ++i) {
                RECT band = client;
                band.left = i * width / static_cast<int>(ARRAYSIZE(colors));
                band.right = (i + 1) * width / static_cast<int>(ARRAYSIZE(colors));
                HBRUSH brush = CreateSolidBrush(colors[i]);
                if (brush != nullptr) {
                    FillRect(dc, &band, brush);
                    DeleteObject(brush);
                }
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT textRect = client;
            textRect.left += 16;
            textRect.top += 16;
            DrawTextW(dc, L"UsbCastReceiver video child window", -1, &textRect, DT_LEFT | DT_TOP | DT_NOPREFIX);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow()
{
    Destroy();
}

HRESULT MainWindow::Create(HINSTANCE instance, int showCommand, IVideoPlayer* videoPlayer, IAudioPlayer* audioPlayer)
{
    instance_ = instance;
    videoPlayer_ = videoPlayer;
    audioPlayer_ = audioPlayer;

    RETURN_IF_FAILED_LOG(RegisterWindowClass(), L"MainWindow::RegisterWindowClass");

    hwnd_ = CreateWindowExW(
        0,
        kMainWindowClassName,
        L"USB Cast Receiver",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        720,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateWindowExW(main)", hr);
        return hr;
    }

    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    return S_OK;
}

void MainWindow::Destroy()
{
    StopMedia();

    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

HRESULT MainWindow::RegisterWindowClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kMainWindowClassName;

    if (RegisterClassExW(&wc) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            const HRESULT hr = HRESULT_FROM_WIN32(error);
            LogHResult(L"RegisterClassExW", hr);
            return hr;
        }
    }

    WNDCLASSEXW videoWc = {};
    videoWc.cbSize = sizeof(videoWc);
    videoWc.lpfnWndProc = VideoWindowProc;
    videoWc.hInstance = instance_;
    videoWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    videoWc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoWc.lpszClassName = kVideoWindowClassName;

    if (RegisterClassExW(&videoWc) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            const HRESULT hr = HRESULT_FROM_WIN32(error);
            LogHResult(L"RegisterClassExW(video)", hr);
            return hr;
        }
    }

    return S_OK;
}

HRESULT MainWindow::OnCreate()
{
    videoHwnd_ = CreateWindowExW(
        0,
        kVideoWindowClassName,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    if (videoHwnd_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateWindowExW(video)", hr);
        return hr;
    }

    RETURN_IF_FAILED_LOG(controls_.Create(hwnd_, instance_), L"OverlayControls::Create");

    RECT client = {};
    GetClientRect(hwnd_, &client);
    OnSize(static_cast<UINT>(client.right - client.left), static_cast<UINT>(client.bottom - client.top));
    return S_OK;
}

void MainWindow::OnSize(UINT width, UINT height)
{
    if (videoHwnd_ != nullptr) {
        MoveWindow(videoHwnd_, 0, 0, static_cast<int>(width), static_cast<int>(height), TRUE);
    }

    RECT client = {};
    client.right = static_cast<LONG>(width);
    client.bottom = static_cast<LONG>(height);
    controls_.Layout(client);
    controls_.UpdateMaximized(hwnd_ != nullptr && IsZoomed(hwnd_));

    if (videoPlayer_ != nullptr) {
        videoPlayer_->Resize(width, height);
    }
}

void MainWindow::OnCommand(int commandId)
{
    switch (commandId) {
    case ID_BTN_MUTE:
        ToggleMute();
        break;
    case ID_BTN_STOP:
        StopMedia();
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
        }
        break;
    case ID_BTN_MAX_RESTORE:
        ToggleMaximizeRestore();
        break;
    default:
        break;
    }
}

void MainWindow::StopMedia()
{
    if (stopping_) {
        return;
    }

    stopping_ = true;

    if (audioPlayer_ != nullptr) {
        audioPlayer_->Stop();
    }

    if (videoPlayer_ != nullptr) {
        videoPlayer_->Stop();
    }

    stopping_ = false;
}

void MainWindow::ToggleMute()
{
    if (audioPlayer_ == nullptr) {
        return;
    }

    const bool muted = !audioPlayer_->IsMuted();
    audioPlayer_->SetMuted(muted);
    controls_.UpdateMuted(muted);
}

void MainWindow::ToggleMaximizeRestore()
{
    if (hwnd_ == nullptr) {
        return;
    }

    if (IsZoomed(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    } else {
        ShowWindow(hwnd_, SW_MAXIMIZE);
    }

    controls_.UpdateMaximized(IsZoomed(hwnd_));
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        if (FAILED(OnCreate())) {
            return -1;
        }
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            OnCommand(LOWORD(wParam));
        }
        return 0;

    case WM_GETMINMAXINFO:
        if (lParam != 0) {
            auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
            minMaxInfo->ptMinTrackSize.x = 480;
            minMaxInfo->ptMinTrackSize.y = 270;
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (hwnd_ != nullptr && IsZoomed(hwnd_)) {
                ShowWindow(hwnd_, SW_RESTORE);
            } else if (hwnd_ != nullptr) {
                DestroyWindow(hwnd_);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        StopMedia();
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY:
        hwnd_ = nullptr;
        videoHwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
