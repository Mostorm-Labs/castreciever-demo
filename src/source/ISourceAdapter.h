#pragma once

#include "core/MediaCore.h"

#include <windows.h>

#include <string>

struct SourceStartContext {
    IMediaSink* mediaSink = nullptr;
    HWND videoHwnd = nullptr;
};

class ISourceAdapter {
public:
    virtual ~ISourceAdapter() = default;
    virtual MediaSourceKind SourceKind() const = 0;
    virtual HRESULT Start(const SourceStartContext& context) = 0;
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
};

