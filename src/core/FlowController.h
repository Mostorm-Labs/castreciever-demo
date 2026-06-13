#pragma once

#include "core/FrameQueue.h"

#include <cstdint>
#include <optional>

struct FlowControlPolicy {
    size_t maxVideoQueueDepth = 3;
    int64_t lateDropNs = 120000000;
    int64_t futureRebaseNs = 200000000;
    int64_t rebaseLeadNs = 10000000;
    int64_t statsIntervalNs = 1000000000;
};

struct FlowControlStats {
    uint64_t submittedVideoFrames = 0;
    uint64_t renderedVideoFrames = 0;
    uint64_t droppedLateVideoFrames = 0;
    uint64_t droppedOldVideoFrames = 0;
    uint64_t rebasedFutureVideoFrames = 0;
    uint64_t submittedAudioFrames = 0;
    size_t videoQueueDepth = 0;
};

class FlowController {
public:
    explicit FlowController(FlowControlPolicy policy = {});

    void Reset();
    bool SubmitVideoFrame(MediaVideoFrame frame, int64_t nowQpcNs);
    void SubmitAudioFrame(const MediaAudioFrame& frame);
    std::optional<MediaVideoFrame> TakeRenderableVideoFrame(int64_t nowQpcNs);
    bool ShouldPublishStats(int64_t nowQpcNs);

    FlowControlStats Stats() const { return stats_; }
    const FlowControlPolicy& Policy() const { return policy_; }

private:
    FlowControlPolicy policy_;
    FrameQueue videoQueue_;
    FlowControlStats stats_;
    int64_t lastStatsQpcNs_ = 0;
};

