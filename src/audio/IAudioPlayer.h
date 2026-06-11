#pragma once

#include <windows.h>

#include <string>

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;
    virtual HRESULT Start(const std::wstring& captureDeviceMatch) = 0;
    virtual void Stop() = 0;
    virtual void SetMuted(bool muted) = 0;
    virtual bool IsMuted() const = 0;
};
