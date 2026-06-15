#include "ui/OverlayControls.h"

#include "HResult.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <windowsx.h>

namespace {

constexpr wchar_t kOverlayControlsClassName[] = L"UsbCastReceiverOverlayControls";
constexpr int kButtonSize = 64;
constexpr int kButtonGap = 20;
constexpr int kOverlayPadding = 14;
constexpr int kOverlayRightMargin = 18;
constexpr double kPi = 3.14159265358979323846;

struct ScopedPen {
    explicit ScopedPen(HPEN value) : pen(value) {}
    ~ScopedPen() { if (pen != nullptr) { DeleteObject(pen); } }
    HPEN pen = nullptr;
};

struct ScopedBrush {
    explicit ScopedBrush(HBRUSH value) : brush(value) {}
    ~ScopedBrush() { if (brush != nullptr) { DeleteObject(brush); } }
    HBRUSH brush = nullptr;
};

RECT ButtonRectForIndex(const RECT& client, int index)
{
    const int height = client.bottom - client.top;
    const int totalHeight = kButtonSize * 3 + kButtonGap * 2;
    const int x = client.left + ((client.right - client.left) - kButtonSize) / 2;
    const int yStart = std::max(0, (height - totalHeight) / 2);
    const int y = yStart + index * (kButtonSize + kButtonGap);
    return RECT { x, y, x + kButtonSize, y + kButtonSize };
}

RECT ButtonRectForCommand(const RECT& client, int commandId)
{
    switch (commandId) {
    case ID_BTN_MUTE:
        return ButtonRectForIndex(client, 0);
    case ID_BTN_MAX_RESTORE:
        return ButtonRectForIndex(client, 1);
    case ID_BTN_STOP:
        return ButtonRectForIndex(client, 2);
    default:
        return RECT {};
    }
}

int CommandForPoint(const RECT& client, POINT point)
{
    const int commands[] = {
        ID_BTN_MUTE,
        ID_BTN_MAX_RESTORE,
        ID_BTN_STOP,
    };

    for (int commandId : commands) {
        RECT rect = ButtonRectForCommand(client, commandId);
        if (PtInRect(&rect, point)) {
            return commandId;
        }
    }

    return 0;
}

void DrawArcPolyline(HDC dc, int centerX, int centerY, int radius, double startDegrees, double sweepDegrees)
{
    std::array<POINT, 40> points = {};
    for (size_t i = 0; i < points.size(); ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(points.size() - 1);
        const double degrees = startDegrees + sweepDegrees * t;
        const double radians = degrees * kPi / 180.0;
        points[i].x = centerX + static_cast<LONG>(std::lround(std::cos(radians) * radius));
        points[i].y = centerY - static_cast<LONG>(std::lround(std::sin(radians) * radius));
    }

    Polyline(dc, points.data(), static_cast<int>(points.size()));
}

void DrawButtonShell(HDC dc, const RECT& rect, bool hovered, bool pressed, bool danger)
{
    COLORREF fill = RGB(24, 24, 24);
    COLORREF outline = RGB(86, 86, 86);
    if (hovered) {
        fill = danger ? RGB(46, 22, 22) : RGB(42, 42, 42);
        outline = danger ? RGB(170, 58, 58) : RGB(112, 112, 112);
    }
    if (pressed) {
        fill = danger ? RGB(68, 26, 26) : RGB(58, 58, 58);
        outline = danger ? RGB(230, 72, 72) : RGB(150, 150, 150);
    }

    ScopedBrush brush(CreateSolidBrush(fill));
    ScopedPen pen(CreatePen(PS_SOLID, 2, outline));
    HGDIOBJ oldBrush = SelectObject(dc, brush.brush);
    HGDIOBJ oldPen = SelectObject(dc, pen.pen);
    Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

int ScaleForButton(const RECT& rect, int value)
{
    const int size = std::max(1, static_cast<int>(rect.right - rect.left));
    return std::max(1, MulDiv(value, size, 46));
}

void DrawAudioIcon(HDC dc, const RECT& rect, bool muted)
{
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const COLORREF iconColor = RGB(245, 245, 245);
    const int penWidth = ScaleForButton(rect, 3);

    ScopedBrush brush(CreateSolidBrush(iconColor));
    ScopedPen pen(CreatePen(PS_SOLID, penWidth, iconColor));
    HGDIOBJ oldBrush = SelectObject(dc, brush.brush);
    HGDIOBJ oldPen = SelectObject(dc, pen.pen);

    POINT speaker[] = {
        { centerX - ScaleForButton(rect, 16), centerY - ScaleForButton(rect, 6) },
        { centerX - ScaleForButton(rect, 9), centerY - ScaleForButton(rect, 6) },
        { centerX, centerY - ScaleForButton(rect, 14) },
        { centerX, centerY + ScaleForButton(rect, 14) },
        { centerX - ScaleForButton(rect, 9), centerY + ScaleForButton(rect, 6) },
        { centerX - ScaleForButton(rect, 16), centerY + ScaleForButton(rect, 6) },
    };
    Polygon(dc, speaker, ARRAYSIZE(speaker));

    if (muted) {
        MoveToEx(dc, centerX + ScaleForButton(rect, 8), centerY - ScaleForButton(rect, 10), nullptr);
        LineTo(dc, centerX + ScaleForButton(rect, 20), centerY + ScaleForButton(rect, 10));
        MoveToEx(dc, centerX + ScaleForButton(rect, 20), centerY - ScaleForButton(rect, 10), nullptr);
        LineTo(dc, centerX + ScaleForButton(rect, 8), centerY + ScaleForButton(rect, 10));
    } else {
        DrawArcPolyline(dc, centerX + ScaleForButton(rect, 2), centerY, ScaleForButton(rect, 11), -42.0, 84.0);
        DrawArcPolyline(dc, centerX + ScaleForButton(rect, 4), centerY, ScaleForButton(rect, 18), -38.0, 76.0);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void DrawFullscreenIcon(HDC dc, const RECT& rect, bool maximized)
{
    const COLORREF iconColor = RGB(245, 245, 245);
    ScopedPen pen(CreatePen(PS_SOLID, ScaleForButton(rect, 3), iconColor));
    HGDIOBJ oldPen = SelectObject(dc, pen.pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    if (maximized) {
        RECT back = {
            rect.left + ScaleForButton(rect, 15),
            rect.top + ScaleForButton(rect, 12),
            rect.right - ScaleForButton(rect, 10),
            rect.bottom - ScaleForButton(rect, 16),
        };
        RECT front = {
            rect.left + ScaleForButton(rect, 10),
            rect.top + ScaleForButton(rect, 17),
            rect.right - ScaleForButton(rect, 15),
            rect.bottom - ScaleForButton(rect, 11),
        };
        RoundRect(dc, back.left, back.top, back.right, back.bottom, ScaleForButton(rect, 3), ScaleForButton(rect, 3));
        RoundRect(dc, front.left, front.top, front.right, front.bottom, ScaleForButton(rect, 3), ScaleForButton(rect, 3));
    } else {
        const int left = rect.left + ScaleForButton(rect, 13);
        const int right = rect.right - ScaleForButton(rect, 13);
        const int top = rect.top + ScaleForButton(rect, 13);
        const int bottom = rect.bottom - ScaleForButton(rect, 13);
        const int leg = ScaleForButton(rect, 8);

        MoveToEx(dc, left, top + leg, nullptr);
        LineTo(dc, left, top);
        LineTo(dc, left + leg, top);

        MoveToEx(dc, right - leg, top, nullptr);
        LineTo(dc, right, top);
        LineTo(dc, right, top + leg);

        MoveToEx(dc, right, bottom - leg, nullptr);
        LineTo(dc, right, bottom);
        LineTo(dc, right - leg, bottom);

        MoveToEx(dc, left + leg, bottom, nullptr);
        LineTo(dc, left, bottom);
        LineTo(dc, left, bottom - leg);
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

void DrawPowerIcon(HDC dc, const RECT& rect)
{
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2 + ScaleForButton(rect, 2);
    const COLORREF iconColor = RGB(255, 70, 70);

    ScopedPen pen(CreatePen(PS_SOLID, ScaleForButton(rect, 4), iconColor));
    HGDIOBJ oldPen = SelectObject(dc, pen.pen);

    DrawArcPolyline(dc, centerX, centerY, ScaleForButton(rect, 13), 140.0, 260.0);
    MoveToEx(dc, centerX, centerY - ScaleForButton(rect, 19), nullptr);
    LineTo(dc, centerX, centerY - ScaleForButton(rect, 4));

    SelectObject(dc, oldPen);
}

HRESULT RegisterOverlayClass(HINSTANCE instance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayControls::WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kOverlayControlsClassName;

    if (RegisterClassExW(&wc) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            const HRESULT hr = HRESULT_FROM_WIN32(error);
            LogHResult(L"RegisterClassExW(overlay controls)", hr);
            return hr;
        }
    }

    return S_OK;
}

void UpdateOverlayRegion(HWND hwnd, int width, int height)
{
    HRGN combinedRegion = CreateRectRgn(0, 0, 0, 0);
    if (combinedRegion == nullptr) {
        LogHResult(L"CreateRectRgn(overlay controls)", HResultFromLastError());
        return;
    }

    RECT client = {};
    client.right = width;
    client.bottom = height;
    const int commands[] = {
        ID_BTN_MUTE,
        ID_BTN_MAX_RESTORE,
        ID_BTN_STOP,
    };

    for (int commandId : commands) {
        const RECT rect = ButtonRectForCommand(client, commandId);
        HRGN buttonRegion = CreateEllipticRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1);
        if (buttonRegion == nullptr) {
            LogHResult(L"CreateEllipticRgn(overlay controls)", HResultFromLastError());
            continue;
        }

        CombineRgn(combinedRegion, combinedRegion, buttonRegion, RGN_OR);
        DeleteObject(buttonRegion);
    }

    if (SetWindowRgn(hwnd, combinedRegion, TRUE) == 0) {
        LogHResult(L"SetWindowRgn(overlay controls)", HResultFromLastError());
        DeleteObject(combinedRegion);
    }
}

} // namespace

HRESULT OverlayControls::Create(HWND parent, HINSTANCE instance)
{
    Log::Checkpoint(L"OverlayControls::Create begin parent=0x%p", parent);
    parent_ = parent;

    RETURN_IF_FAILED_LOG(RegisterOverlayClass(instance), L"RegisterOverlayClass");
    Log::Checkpoint(L"CreateWindowExW(overlay controls)");

    hwnd_ = CreateWindowExW(
        0,
        kOverlayControlsClassName,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        parent,
        nullptr,
        instance,
        this);
    if (hwnd_ == nullptr) {
        const HRESULT hr = HResultFromLastError();
        LogHResult(L"CreateWindowExW(overlay controls)", hr);
        return hr;
    }
    Log::Write(L"Overlay controls window handle created: hwnd=0x%p parent=0x%p", hwnd_, parent_);

    return S_OK;
}

void OverlayControls::Layout(const RECT& client)
{
    if (hwnd_ == nullptr) {
        return;
    }

    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    const int desiredWidth = kButtonSize + kOverlayPadding * 2;
    const int desiredHeight = kButtonSize * 3 + kButtonGap * 2 + kOverlayPadding * 2;
    const int width = std::min(desiredWidth, std::max(0, clientWidth));
    const int height = std::min(desiredHeight, std::max(0, clientHeight));
    const int x = std::max(0, clientWidth - width - kOverlayRightMargin);
    const int y = std::max(0, (clientHeight - height) / 2);

    static UINT loggedLayoutCount = 0;
    if (loggedLayoutCount < 8) {
        ++loggedLayoutCount;
        Log::Write(L"OverlayControls::Layout #%u: parentClient=%dx%d overlay=(%d,%d %dx%d)",
            loggedLayoutCount,
            clientWidth,
            clientHeight,
            x,
            y,
            width,
            height);
    }

    MoveWindow(hwnd_, x, y, width, height, TRUE);
    UpdateOverlayRegion(hwnd_, width, height);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void OverlayControls::UpdateMuted(bool muted)
{
    muted_ = muted;
    Invalidate();
}

void OverlayControls::UpdateMaximized(bool maximized)
{
    maximized_ = maximized;
    Invalidate();
}

LRESULT CALLBACK OverlayControls::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    OverlayControls* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayControls*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<OverlayControls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT OverlayControls::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCHITTEST: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd_, &point);
        return HitTest(point) != 0 ? HTCLIENT : HTTRANSPARENT;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        static bool firstPaintLogged = false;
        if (!firstPaintLogged) {
            firstPaintLogged = true;
            Log::Write(L"Overlay controls first WM_PAINT: hwnd=0x%p hdc=0x%p rcPaint=(%ld,%ld)-(%ld,%ld)",
                hwnd,
                dc,
                paint.rcPaint.left,
                paint.rcPaint.top,
                paint.rcPaint.right,
                paint.rcPaint.bottom);
        }
        Paint(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;

    case WM_MOUSEMOVE: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hoverCommandId = HitTest(point);
        if (hoverCommandId != hoverCommandId_) {
            hoverCommandId_ = hoverCommandId;
            Invalidate();
        }

        if (!trackingMouse_) {
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd_;
            if (TrackMouseEvent(&track)) {
                trackingMouse_ = true;
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        hoverCommandId_ = 0;
        Invalidate();
        return 0;

    case WM_LBUTTONDOWN: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        pressedCommandId_ = HitTest(point);
        if (pressedCommandId_ != 0) {
            SetCapture(hwnd_);
            Invalidate();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int releasedCommandId = HitTest(point);
        const int clickedCommandId = pressedCommandId_;
        pressedCommandId_ = 0;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        Invalidate();

        if (clickedCommandId != 0 && clickedCommandId == releasedCommandId && parent_ != nullptr) {
            SendMessageW(
                parent_,
                WM_COMMAND,
                MAKEWPARAM(static_cast<WORD>(clickedCommandId), BN_CLICKED),
                reinterpret_cast<LPARAM>(hwnd_));
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        pressedCommandId_ = 0;
        Invalidate();
        return 0;

    case WM_NCDESTROY:
        hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void OverlayControls::Paint(HDC dc)
{
    RECT client = {};
    GetClientRect(hwnd_, &client);

    const int commands[] = {
        ID_BTN_MUTE,
        ID_BTN_MAX_RESTORE,
        ID_BTN_STOP,
    };

    SetBkMode(dc, TRANSPARENT);
    for (int commandId : commands) {
        const RECT rect = ButtonRectForCommand(client, commandId);
        const bool danger = commandId == ID_BTN_STOP;
        DrawButtonShell(dc, rect, hoverCommandId_ == commandId, pressedCommandId_ == commandId, danger);

        switch (commandId) {
        case ID_BTN_MUTE:
            DrawAudioIcon(dc, rect, muted_);
            break;
        case ID_BTN_MAX_RESTORE:
            DrawFullscreenIcon(dc, rect, maximized_);
            break;
        case ID_BTN_STOP:
            DrawPowerIcon(dc, rect);
            break;
        default:
            break;
        }
    }
}

int OverlayControls::HitTest(POINT point) const
{
    RECT client = {};
    GetClientRect(hwnd_, &client);
    return CommandForPoint(client, point);
}

void OverlayControls::Invalidate()
{
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
