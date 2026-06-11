#include "video/SelfTestVideoPlayer.h"

#include "Log.h"

#include <strsafe.h>

namespace {

constexpr wchar_t kSelfTestPaintProperty[] = L"UsbCastReceiverSelfTestPaint";

void PaintSelfTest(HWND hwnd, int tick, const wchar_t* surfaceName)
{
    if (hwnd == nullptr) {
        return;
    }

    RECT client = {};
    if (!GetClientRect(hwnd, &client)) {
        return;
    }

    HDC dc = GetDC(hwnd);
    if (dc == nullptr) {
        return;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const COLORREF colors[] = {
        RGB(255, 0, 0),
        RGB(0, 180, 0),
        RGB(0, 80, 255),
        RGB(255, 220, 0),
        RGB(200, 0, 200),
    };

    for (int i = 0; i < static_cast<int>(ARRAYSIZE(colors)); ++i) {
        RECT band = client;
        band.left = i * width / static_cast<int>(ARRAYSIZE(colors));
        band.right = (i + 1) * width / static_cast<int>(ARRAYSIZE(colors));
        HBRUSH brush = CreateSolidBrush(colors[(i + tick) % ARRAYSIZE(colors)]);
        if (brush != nullptr) {
            FillRect(dc, &band, brush);
            DeleteObject(brush);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    wchar_t text[256] = {};
    StringCchPrintfW(text, ARRAYSIZE(text), L"SELF TEST %s\nIf this is not visible, Win32 painting is not reaching the displayed surface.\nTick: %d", surfaceName, tick);

    RECT textRect = client;
    textRect.left += 24;
    textRect.top += 24;
    DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    ReleaseDC(hwnd, dc);
}

} // namespace

SelfTestVideoPlayer::SelfTestVideoPlayer() = default;

SelfTestVideoPlayer::~SelfTestVideoPlayer()
{
    Stop();
}

HRESULT SelfTestVideoPlayer::Start(HWND hwndVideo, const VideoStartOptions&)
{
    if (hwndVideo == nullptr) {
        return E_INVALIDARG;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return S_OK;
    }

    hwndVideo_ = hwndVideo;
    HWND parent = GetParent(hwndVideo_);
    SetPropW(hwndVideo_, kSelfTestPaintProperty, reinterpret_cast<HANDLE>(1));
    if (parent != nullptr) {
        SetPropW(parent, kSelfTestPaintProperty, reinterpret_cast<HANDLE>(1));
    }

    RECT videoRect = {};
    RECT parentRect = {};
    GetClientRect(hwndVideo_, &videoRect);
    if (parent != nullptr) {
        GetClientRect(parent, &parentRect);
    }

    Log::Write(L"Self-test video backend started. videoHwnd=0x%p parentHwnd=0x%p videoClient=%ldx%ld parentClient=%ldx%ld",
        hwndVideo_,
        parent,
        videoRect.right - videoRect.left,
        videoRect.bottom - videoRect.top,
        parentRect.right - parentRect.left,
        parentRect.bottom - parentRect.top);

    InvalidateRect(hwndVideo_, nullptr, TRUE);
    UpdateWindow(hwndVideo_);
    if (parent != nullptr) {
        InvalidateRect(parent, nullptr, TRUE);
        UpdateWindow(parent);
    }

    worker_ = std::thread(&SelfTestVideoPlayer::WorkerThread, this);
    return S_OK;
}

void SelfTestVideoPlayer::Stop()
{
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (hwndVideo_ != nullptr) {
        HWND parent = GetParent(hwndVideo_);
        RemovePropW(hwndVideo_, kSelfTestPaintProperty);
        if (parent != nullptr) {
            RemovePropW(parent, kSelfTestPaintProperty);
        }
    }
    hwndVideo_ = nullptr;
}

void SelfTestVideoPlayer::Resize(UINT, UINT)
{
    PaintSelfTest(hwndVideo_, 0, L"VIDEO WINDOW");
    PaintSelfTest(GetParent(hwndVideo_), 0, L"MAIN WINDOW");
}

void SelfTestVideoPlayer::WorkerThread()
{
    int tick = 0;
    while (running_.load()) {
        PaintSelfTest(hwndVideo_, tick, L"VIDEO WINDOW");
        PaintSelfTest(GetParent(hwndVideo_), tick, L"MAIN WINDOW");
        ++tick;
        Sleep(500);
    }
}
