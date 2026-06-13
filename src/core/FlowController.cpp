#include "core/FlowController.h"

#include <utility>

FlowController::FlowController(FlowControlPolicy policy)
    : policy_(policy)
    , videoQueue_(policy.maxVideoQueueDepth)
{
}

void FlowController::Reset()
{
    videoQueue_.Clear();
    stats_ = {};
    lastStatsQpcNs_ = 0;
}

bool FlowController::SubmitVideoFrame(MediaVideoFrame frame, int64_t nowQpcNs)
{
    ++stats_.submittedVideoFrames;

    if (nowQpcNs - frame.ptsQpcNs > policy_.lateDropNs) {
        ++stats_.droppedLateVideoFrames;
        return false;
    }

    if (frame.ptsQpcNs - nowQpcNs > policy_.futureRebaseNs) {
        frame.ptsQpcNs = nowQpcNs + policy_.rebaseLeadNs;
        ++stats_.rebasedFutureVideoFrames;
    }

    const uint64_t queueLateDrops = videoQueue_.DropLate(nowQpcNs, policy_.lateDropNs);
    stats_.droppedLateVideoFrames += queueLateDrops;

    const FrameQueuePushResult push = videoQueue_.PushDropOld(std::move(frame));
    stats_.droppedOldVideoFrames += push.droppedOldFrames;
    stats_.videoQueueDepth = push.depth;
    return true;
}

void FlowController::SubmitAudioFrame(const MediaAudioFrame&)
{
    ++stats_.submittedAudioFrames;
}

std::optional<MediaVideoFrame> FlowController::TakeRenderableVideoFrame(int64_t nowQpcNs)
{
    const uint64_t queueLateDrops = videoQueue_.DropLate(nowQpcNs, policy_.lateDropNs);
    stats_.droppedLateVideoFrames += queueLateDrops;

    auto frame = videoQueue_.TakeLatestReady(nowQpcNs);
    if (frame) {
        ++stats_.renderedVideoFrames;
    }
    stats_.videoQueueDepth = videoQueue_.Depth();
    return frame;
}

bool FlowController::ShouldPublishStats(int64_t nowQpcNs)
{
    if (lastStatsQpcNs_ == 0) {
        lastStatsQpcNs_ = nowQpcNs;
        return true;
    }

    if (nowQpcNs - lastStatsQpcNs_ < policy_.statsIntervalNs) {
        return false;
    }

    lastStatsQpcNs_ = nowQpcNs;
    return true;
}
