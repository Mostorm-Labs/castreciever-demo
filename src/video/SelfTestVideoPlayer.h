#pragma once

#include "video/IVideoPlayer.h"

#include <atomic>
#include <thread>

class SelfTestVideoPlayer final : public IVideoPlayer {
public:
    SelfTestVideoPlayer();
    ~SelfTestVideoPlayer() override;

    HRESULT Start(HWND hwndVideo, const VideoStartOptions& options) override;
    void Stop() override;
    void Resize(UINT width, UINT height) override;

private:
    void WorkerThread();

    HWND hwndVideo_ = nullptr;
    std::thread worker_;
    std::atomic_bool running_{ false };
};
