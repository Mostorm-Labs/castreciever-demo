#pragma once

#include "core/MediaTypes.h"

#include <cstddef>
#include <deque>
#include <optional>

struct FrameQueuePushResult {
    uint64_t droppedOldFrames = 0;
    size_t depth = 0;
};

class FrameQueue {
public:
    explicit FrameQueue(size_t maxDepth = 3);

    void Clear();
    FrameQueuePushResult PushDropOld(MediaVideoFrame frame);
    uint64_t DropLate(int64_t nowQpcNs, int64_t maxLateNs);
    std::optional<MediaVideoFrame> TakeLatestReady(int64_t nowQpcNs);

    size_t Depth() const { return frames_.size(); }
    size_t MaxDepth() const { return maxDepth_; }

private:
    size_t maxDepth_ = 3;
    std::deque<MediaVideoFrame> frames_;
};

