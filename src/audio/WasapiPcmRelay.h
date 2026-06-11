#pragma once

#include "audio/IAudioPlayer.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class WasapiPcmRelay final : public IAudioPlayer {
public:
    WasapiPcmRelay();
    ~WasapiPcmRelay() override;

    HRESULT Start(const std::wstring& captureDeviceMatch) override;
    void Stop() override;
    void SetMuted(bool muted) override;
    bool IsMuted() const override;

private:
    void WorkerThread();
    void SignalInitialized(HRESULT hr);

    std::wstring captureDeviceMatch_;
    std::thread worker_;
    std::atomic_bool running_{ false };
    std::atomic_bool muted_{ false };

    std::mutex initMutex_;
    std::condition_variable initCondition_;
    bool initDone_ = false;
    HRESULT initHr_ = E_PENDING;
};
