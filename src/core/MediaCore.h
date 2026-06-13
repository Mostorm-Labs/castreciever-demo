#pragma once

#include "core/FlowController.h"

#include <windows.h>

#include <functional>
#include <mutex>
#include <optional>

class IMediaSink {
public:
    virtual ~IMediaSink() = default;
    virtual HRESULT SubmitVideo(MediaVideoFrame frame) = 0;
    virtual HRESULT SubmitAudio(MediaAudioFrame frame) = 0;
    virtual void NotifyEvent(MediaEvent event) = 0;
};

class MediaCore final : public IMediaSink {
public:
    using EventCallback = std::function<void(const MediaEvent&)>;

    explicit MediaCore(FlowControlPolicy policy = {});

    bool StartSession(MediaSourceKind source);
    void EndSession(MediaSourceKind source);
    MediaSourceKind ActiveSource() const;

    HRESULT SubmitVideo(MediaVideoFrame frame) override;
    HRESULT SubmitAudio(MediaAudioFrame frame) override;
    void NotifyEvent(MediaEvent event) override;

    std::optional<MediaVideoFrame> TakeRenderableVideoFrame();
    FlowControlStats Stats() const;
    void SetEventCallback(EventCallback callback);

private:
    mutable std::mutex mutex_;
    FlowController flow_;
    MediaSourceKind activeSource_ = MediaSourceKind::Unknown;
    EventCallback eventCallback_;
};
