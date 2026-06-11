#pragma once

#include <windows.h>

#include <string>

enum class PreviewSinkMode {
    Default,
    AutoAddStream,
    Rgb32AddStream,
};

enum class VideoBackend {
    CaptureEngine,
    SourceReader,
};

struct VideoStartOptions {
    std::wstring deviceMatch;
    bool preferH264 = true;
    PreviewSinkMode previewSinkMode = PreviewSinkMode::Default;
};

class IVideoPlayer {
public:
    virtual ~IVideoPlayer() = default;
    virtual HRESULT Start(HWND hwndVideo, const VideoStartOptions& options) = 0;
    virtual void Stop() = 0;
    virtual void Resize(UINT width, UINT height) = 0;
};
