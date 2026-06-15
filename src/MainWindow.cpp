#include "MainWindow.h"

#include "AppMessages.h"
#include "HResult.h"
#include "Log.h"
#include "audio/IAudioPlayer.h"
#include "video/IVideoPlayer.h"

#include <algorithm>
#include <strsafe.h>
#include <windowsx.h>

namespace {

constexpr wchar_t kMainWindowClassName[] = L"UsbCastReceiverMainWindow";
constexpr wchar_t kVideoWindowClassName[] = L"UsbCastReceiverVideoWindow";
constexpr wchar_t kStatsWindowClassName[] = L"UsbCastReceiverStatsWindow";
constexpr wchar_t kSelfTestPaintProperty[] = L"UsbCastReceiverSelfTestPaint";
constexpr wchar_t kMainPaintLoggedProperty[] = L"UsbCastReceiverMainPaintLogged";
constexpr wchar_t kVideoPaintLoggedProperty[] = L"UsbCastReceiverVideoPaintLogged";
constexpr wchar_t kStatsPaintLoggedProperty[] = L"UsbCastReceiverStatsPaintLogged";
constexpr wchar_t kStatsFpsProperty[] = L"UsbCastReceiverStatsFpsTenths";
constexpr wchar_t kStatsFrameCountProperty[] = L"UsbCastReceiverStatsFrameCount";
constexpr int kMinWindowWidth = 480;
constexpr int kMinWindowHeight = 270;
constexpr int kResizeBorderWidth = 8;
constexpr int kStatsOverlayWidth = 168;
constexpr int kStatsOverlayHeight = 56;
constexpr int kStatsOverlayMargin = 12;
constexpr int kDefaultWindowWidth = 1280;
constexpr int kDefaultWindowHeight = 720;

POINT ScreenPointFromLParam(LPARAM lParam)
{
    return POINT { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
}

void LogFirstPaint(HWND hwnd, const wchar_t* paintProperty, const wchar_t* name, HDC dc, const PAINTSTRUCT& paint)
{
    if (GetPropW(hwnd, paintProperty) != nullptr) {
        return;
    }

    SetPropW(hwnd, paintProperty, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1)));
    Log::Write(L"%s first WM_PAINT: hwnd=0x%p hdc=0x%p rcPaint=(%ld,%ld)-(%ld,%ld)",
        name,
        hwnd,
        dc,
        paint.rcPaint.left,
        paint.rcPaint.top,
        paint.rcPaint.right,
        paint.rcPaint.bottom);
}

LRESULT HitTestBorderlessWindow(HWND hwnd, POINT point)
{
    if (IsZoomed(hwnd)) {
        return HTCLIENT;
    }

    RECT window = {};
    if (!GetWindowRect(hwnd, &window)) {
        return HTCLIENT;
    }

    const int resizeBorderWidth = std::max(kResizeBorderWidth, GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER));
    const bool left = point.x >= window.left && point.x < window.left + resizeBorderWidth;
    const bool right = point.x < window.right && point.x >= window.right - resizeBorderWidth;
    const bool top = point.y >= window.top && point.y < window.top + resizeBorderWidth;
    const bool bottom = point.y < window.bottom && point.y >= window.bottom - resizeBorderWidth;

    if (top && left) {
        return HTTOPLEFT;
    }
    if (top && right) {
        return HTTOPRIGHT;
    }
    if (bottom && left) {
        return HTBOTTOMLEFT;
    }
    if (bottom && right) {
        return HTBOTTOMRIGHT;
    }
    if (left) {
        return HTLEFT;
    }
    if (right) {
        return HTRIGHT;
    }
    if (top) {
        return HTTOP;
    }
    if (bottom) {
        return HTBOTTOM;
    }

    return HTCAPTION;
}

void CenterWindowOnMonitor(HWND hwnd, UINT targetWidth, UINT targetHeight)
{
    if (hwnd == nullptr || targetWidth == 0 || targetHeight == 0) {
        return;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }

    const int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    UINT finalWidth = targetWidth;
    UINT finalHeight = targetHeight;

    if (static_cast<int>(finalWidth) > monitorWidth || static_cast<int>(finalHeight) > monitorHeight) {
        const double scaleX = static_cast<double>(monitorWidth) / static_cast<double>(targetWidth);
        const double scaleY = static_cast<double>(monitorHeight) / static_cast<double>(targetHeight);
        const double scale = std::min(scaleX, scaleY);
        finalWidth = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(targetWidth) * scale));
        finalHeight = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(targetHeight) * scale));
        Log::Write(L"Native video size %ux%u is larger than monitor %dx%d; fitting window to %ux%u.",
            targetWidth,
            targetHeight,
            monitorWidth,
            monitorHeight,
            finalWidth,
            finalHeight);
    }

    const int x = monitorInfo.rcMonitor.left + (monitorWidth - static_cast<int>(finalWidth)) / 2;
    const int y = monitorInfo.rcMonitor.top + (monitorHeight - static_cast<int>(finalHeight)) / 2;
    SetWindowPos(
        hwnd,
        nullptr,
        x,
        y,
        static_cast<int>(finalWidth),
        static_cast<int>(finalHeight),
        SWP_NOZORDER | SWP_NOACTIVATE);
}

void PaintSelfTestPattern(HWND hwnd, HDC dc, const wchar_t* label)
{
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
    DrawTextW(dc, label, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

LRESULT CALLBACK VideoWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        LogFirstPaint(hwnd, kVideoPaintLoggedProperty, L"Video child window", dc, paint);
        if (GetPropW(hwnd, kSelfTestPaintProperty) != nullptr) {
            PaintSelfTestPattern(hwnd, dc, L"UsbCastReceiver video child window");
        }
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_NCDESTROY:
        RemovePropW(hwnd, kVideoPaintLoggedProperty);
        break;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

LRESULT CALLBACK StatsWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        LogFirstPaint(hwnd, kStatsPaintLoggedProperty, L"Stats overlay window", dc, paint);

        RECT client = {};
        GetClientRect(hwnd, &client);

        HBRUSH background = CreateSolidBrush(RGB(18, 20, 22));
        if (background != nullptr) {
            FillRect(dc, &client, background);
            DeleteObject(background);
        }

        SetBkMode(dc, TRANSPARENT);

        HFONT labelFont = CreateFontW(
            13,
            0,
            0,
            0,
            FW_MEDIUM,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS,
            L"Segoe UI");
        HFONT valueFont = CreateFontW(
            22,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS,
            L"Segoe UI");

        HANDLE fpsProp = GetPropW(hwnd, kStatsFpsProperty);
        HANDLE frameProp = GetPropW(hwnd, kStatsFrameCountProperty);
        UINT fpsTenths = fpsProp != nullptr ? static_cast<UINT>(reinterpret_cast<UINT_PTR>(fpsProp) - 1) : 0;
        UINT frameCount = frameProp != nullptr ? static_cast<UINT>(reinterpret_cast<UINT_PTR>(frameProp) - 1) : 0;

        wchar_t fpsText[32] = {};
        StringCchPrintfW(fpsText, ARRAYSIZE(fpsText), L"%u.%u FPS", fpsTenths / 10, fpsTenths % 10);

        wchar_t frameText[48] = {};
        StringCchPrintfW(frameText, ARRAYSIZE(frameText), L"frames %u", frameCount);

        RECT labelRect = client;
        labelRect.left += 12;
        labelRect.top += 6;
        labelRect.right -= 12;

        HGDIOBJ oldFont = nullptr;
        if (labelFont != nullptr) {
            oldFont = SelectObject(dc, labelFont);
        }
        SetTextColor(dc, RGB(160, 168, 176));
        DrawTextW(dc, L"RENDER", -1, &labelRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        RECT valueRect = client;
        valueRect.left += 12;
        valueRect.top += 20;
        valueRect.right -= 12;
        if (valueFont != nullptr) {
            SelectObject(dc, valueFont);
        }
        SetTextColor(dc, RGB(112, 232, 136));
        DrawTextW(dc, fpsText, -1, &valueRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        RECT frameRect = client;
        frameRect.left += 12;
        frameRect.top += 42;
        frameRect.right -= 12;
        if (labelFont != nullptr) {
            SelectObject(dc, labelFont);
        }
        SetTextColor(dc, RGB(210, 214, 218));
        DrawTextW(dc, frameText, -1, &frameRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
        if (labelFont != nullptr) {
            DeleteObject(labelFont);
        }
        if (valueFont != nullptr) {
            DeleteObject(valueFont);
        }

        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_NCDESTROY:
        RemovePropW(hwnd, kStatsPaintLoggedProperty);
        RemovePropW(hwnd, kStatsFpsProperty);
        RemovePropW(hwnd, kStatsFrameCountProperty);
        break;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow()
{
    Destroy();
}

HRESULT MainWindow::Create(HINSTANCE instance, int showCommand, IVideoPlayer* videoPlayer, IAudioPlayer* audioPlayer)
{
    Log::Checkpoint(L"MainWindow::Create begin");
    instance_ = instance;
    videoPlayer_ = videoPlayer;
    audioPlayer_ = audioPlayer;

    RETURN_IF_FAILED_LOG(RegisterWindowClass(), L"MainWindow::RegisterWindowClass");
    Log::Checkpoint(L"CreateWindowExW(main)");

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kMainWindowClassName,
        L"USB Cast Receiver",
        WS_POPUP | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kDefaultWindowWidth,
        kDefaultWindowHeight,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateWindowExW(main)", hr);
        return hr;
    }
    Log::Write(L"Main window handle created: hwnd=0x%p style=0x%08lX exStyle=0x%08lX defaultSize=%dx%d",
        hwnd_,
        static_cast<unsigned long>(GetWindowLongPtrW(hwnd_, GWL_STYLE)),
        static_cast<unsigned long>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)),
        kDefaultWindowWidth,
        kDefaultWindowHeight);

    Log::Checkpoint(L"ShowWindow/UpdateWindow main hwnd=0x%p", hwnd_);
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    Log::Write(L"Main window shown and updated. showCommand=%d", showCommand);
    return S_OK;
}

void MainWindow::Destroy()
{
    Log::Checkpoint(L"MainWindow::Destroy");
    StopMedia();

    if (hwnd_ != nullptr) {
        Log::Write(L"DestroyWindow(main) hwnd=0x%p", hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

HRESULT MainWindow::RegisterWindowClass()
{
    Log::Checkpoint(L"RegisterWindowClass(main/video/stats)");
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
        Log::Write(L"Main window class already exists.");
    } else {
        Log::Write(L"Registered main window class.");
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
        Log::Write(L"Video child window class already exists.");
    } else {
        Log::Write(L"Registered video child window class.");
    }

    WNDCLASSEXW statsWc = {};
    statsWc.cbSize = sizeof(statsWc);
    statsWc.lpfnWndProc = StatsWindowProc;
    statsWc.hInstance = instance_;
    statsWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    statsWc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    statsWc.lpszClassName = kStatsWindowClassName;

    if (RegisterClassExW(&statsWc) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            const HRESULT hr = HRESULT_FROM_WIN32(error);
            LogHResult(L"RegisterClassExW(stats)", hr);
            return hr;
        }
        Log::Write(L"Stats overlay window class already exists.");
    } else {
        Log::Write(L"Registered stats overlay window class.");
    }

    return S_OK;
}

HRESULT MainWindow::OnCreate()
{
    Log::Checkpoint(L"MainWindow::OnCreate create video child");
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
    Log::Write(L"Video child window handle created: hwnd=0x%p parent=0x%p", videoHwnd_, hwnd_);

    Log::Checkpoint(L"MainWindow::OnCreate create stats overlay");
    statsHwnd_ = CreateWindowExW(
        0,
        kStatsWindowClassName,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        kStatsOverlayMargin,
        kStatsOverlayMargin,
        kStatsOverlayWidth,
        kStatsOverlayHeight,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    if (statsHwnd_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateWindowExW(stats)", hr);
        return hr;
    }
    Log::Write(L"Stats overlay window handle created: hwnd=0x%p parent=0x%p", statsHwnd_, hwnd_);

    Log::Checkpoint(L"OverlayControls::Create");
    RETURN_IF_FAILED_LOG(controls_.Create(hwnd_, instance_), L"OverlayControls::Create");

    RECT client = {};
    GetClientRect(hwnd_, &client);
    Log::Write(L"MainWindow::OnCreate initial client=%ldx%ld", client.right - client.left, client.bottom - client.top);
    OnSize(static_cast<UINT>(client.right - client.left), static_cast<UINT>(client.bottom - client.top));
    Log::Checkpoint(L"MainWindow::OnCreate complete");
    return S_OK;
}

void MainWindow::OnSize(UINT width, UINT height)
{
    static UINT loggedSizeCount = 0;
    if (loggedSizeCount < 8) {
        ++loggedSizeCount;
        Log::Write(L"MainWindow::OnSize #%u: %ux%u hwnd=0x%p videoHwnd=0x%p statsHwnd=0x%p",
            loggedSizeCount,
            width,
            height,
            hwnd_,
            videoHwnd_,
            statsHwnd_);
    }

    if (videoHwnd_ != nullptr) {
        MoveWindow(videoHwnd_, 0, 0, static_cast<int>(width), static_cast<int>(height), TRUE);
    }

    if (statsHwnd_ != nullptr) {
        SetWindowPos(
            statsHwnd_,
            HWND_TOP,
            kStatsOverlayMargin,
            kStatsOverlayMargin,
            kStatsOverlayWidth,
            kStatsOverlayHeight,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
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

void MainWindow::OnVideoStats(UINT fpsTenths, UINT frameCount)
{
    statsFpsTenths_ = fpsTenths;
    statsFrameCount_ = frameCount;

    if (statsHwnd_ != nullptr) {
        SetPropW(statsHwnd_, kStatsFpsProperty, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(statsFpsTenths_) + 1));
        SetPropW(statsHwnd_, kStatsFrameCountProperty, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(statsFrameCount_) + 1));
        InvalidateRect(statsHwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnVideoNativeSize(UINT width, UINT height)
{
    if (width == 0 || height == 0) {
        return;
    }

    const bool nativeSizeChanged = width != nativeVideoWidth_ || height != nativeVideoHeight_;
    nativeVideoWidth_ = width;
    nativeVideoHeight_ = height;
    if (nativeSizeChanged) {
        nativeSizeApplied_ = false;
    }

    if (hwnd_ == nullptr || nativeSizeApplied_ || IsZoomed(hwnd_)) {
        return;
    }

    RECT client = {};
    if (GetClientRect(hwnd_, &client)) {
        const UINT clientWidth = static_cast<UINT>(std::max<LONG>(0, client.right - client.left));
        const UINT clientHeight = static_cast<UINT>(std::max<LONG>(0, client.bottom - client.top));
        if (clientWidth == width && clientHeight == height) {
            nativeSizeApplied_ = true;
            Log::Write(L"Main window already matches native video size: %ux%u.", width, height);
            return;
        }
    }

    nativeSizeApplied_ = true;
    Log::Write(L"Adjusting main window client to native video size: %ux%u.", width, height);
    CenterWindowOnMonitor(hwnd_, width, height);
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

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        LogFirstPaint(hwnd, kMainPaintLoggedProperty, L"Main window", dc, paint);
        if (GetPropW(hwnd, kSelfTestPaintProperty) != nullptr) {
            PaintSelfTestPattern(hwnd, dc, L"UsbCastReceiver main window");
        }
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            OnCommand(LOWORD(wParam));
        }
        return 0;

    case WM_USB_CAST_VIDEO_STATS:
        OnVideoStats(static_cast<UINT>(wParam), static_cast<UINT>(lParam));
        return 0;

    case WM_USB_CAST_VIDEO_NATIVE_SIZE:
        OnVideoNativeSize(static_cast<UINT>(wParam), static_cast<UINT>(lParam));
        return 0;

    case WM_GETMINMAXINFO:
        if (lParam != 0) {
            auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
            minMaxInfo->ptMinTrackSize.x = kMinWindowWidth;
            minMaxInfo->ptMinTrackSize.y = kMinWindowHeight;

            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo = {};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (GetMonitorInfoW(monitor, &monitorInfo)) {
                minMaxInfo->ptMaxPosition.x = 0;
                minMaxInfo->ptMaxPosition.y = 0;
                minMaxInfo->ptMaxSize.x = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
                minMaxInfo->ptMaxSize.y = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
            }
        }
        return 0;

    case WM_NCHITTEST:
        return HitTestBorderlessWindow(hwnd, ScreenPointFromLParam(lParam));

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
        RemovePropW(hwnd, kMainPaintLoggedProperty);
        hwnd_ = nullptr;
        videoHwnd_ = nullptr;
        statsHwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
