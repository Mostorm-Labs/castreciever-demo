#pragma once

#include <windows.h>

#include <string>

class IVideoPlayer {
public:
    virtual ~IVideoPlayer() = default;
    virtual HRESULT Start(HWND hwndVideo, const std::wstring& deviceMatch) = 0;
    virtual void Stop() = 0;
    virtual void Resize(UINT width, UINT height) = 0;
};
