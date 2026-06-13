#include "core/MediaCore.h"

#include "core/ClockMapper.h"
#include "HResult.h"

#include <utility>

MediaCore::MediaCore(FlowControlPolicy policy)
    : flow_(policy)
{
}

bool MediaCore::StartSession(MediaSourceKind source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSource_ != MediaSourceKind::Unknown && activeSource_ != source) {
        return false;
    }

    activeSource_ = source;
    flow_.Reset();
    return true;
}

void MediaCore::EndSession(MediaSourceKind source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSource_ == source || source == MediaSourceKind::Unknown) {
        activeSource_ = MediaSourceKind::Unknown;
        flow_.Reset();
    }
}

MediaSourceKind MediaCore::ActiveSource() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return activeSource_;
}

HRESULT MediaCore::SubmitVideo(MediaVideoFrame frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSource_ != MediaSourceKind::Unknown && activeSource_ != frame.source) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    if (activeSource_ == MediaSourceKind::Unknown) {
        activeSource_ = frame.source;
    }

    flow_.SubmitVideoFrame(std::move(frame), ClockMapper::NowQpcNs());
    return S_OK;
}

HRESULT MediaCore::SubmitAudio(MediaAudioFrame frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSource_ != MediaSourceKind::Unknown && activeSource_ != frame.source) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    if (activeSource_ == MediaSourceKind::Unknown) {
        activeSource_ = frame.source;
    }

    flow_.SubmitAudioFrame(frame);
    return S_OK;
}

void MediaCore::NotifyEvent(MediaEvent event)
{
    EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = eventCallback_;
    }

    if (callback) {
        callback(event);
    }
}

std::optional<MediaVideoFrame> MediaCore::TakeRenderableVideoFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return flow_.TakeRenderableVideoFrame(ClockMapper::NowQpcNs());
}

FlowControlStats MediaCore::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return flow_.Stats();
}

void MediaCore::SetEventCallback(EventCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    eventCallback_ = std::move(callback);
}
