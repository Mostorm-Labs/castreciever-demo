#include "core/FrameQueue.h"

#include <algorithm>
#include <utility>

FrameQueue::FrameQueue(size_t maxDepth)
    : maxDepth_(std::max<size_t>(1, maxDepth))
{
}

void FrameQueue::Clear()
{
    frames_.clear();
}

FrameQueuePushResult FrameQueue::PushDropOld(MediaVideoFrame frame)
{
    FrameQueuePushResult result;

    while (frames_.size() >= maxDepth_) {
        frames_.pop_front();
        ++result.droppedOldFrames;
    }

    frames_.push_back(std::move(frame));
    result.depth = frames_.size();
    return result;
}

uint64_t FrameQueue::DropLate(int64_t nowQpcNs, int64_t maxLateNs)
{
    uint64_t dropped = 0;
    while (!frames_.empty() && nowQpcNs - frames_.front().ptsQpcNs > maxLateNs) {
        frames_.pop_front();
        ++dropped;
    }
    return dropped;
}

std::optional<MediaVideoFrame> FrameQueue::TakeLatestReady(int64_t nowQpcNs)
{
    if (frames_.empty() || frames_.front().ptsQpcNs > nowQpcNs) {
        return std::nullopt;
    }

    size_t readyCount = 0;
    while (readyCount < frames_.size() && frames_[readyCount].ptsQpcNs <= nowQpcNs) {
        ++readyCount;
    }

    MediaVideoFrame selected = std::move(frames_[readyCount - 1]);
    frames_.erase(frames_.begin(), frames_.begin() + static_cast<std::ptrdiff_t>(readyCount));
    return selected;
}
