#include "source/SessionManager.h"

#include <utility>

bool SessionManager::TryBegin(MediaSourceKind source, std::wstring remoteName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SessionState::Idle && activeSource_ != source) {
        return false;
    }

    activeSource_ = source;
    state_ = StateForSource(source);
    remoteName_ = std::move(remoteName);
    return true;
}

void SessionManager::End(MediaSourceKind source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSource_ == source || source == MediaSourceKind::Unknown) {
        activeSource_ = MediaSourceKind::Unknown;
        state_ = SessionState::Idle;
        remoteName_.clear();
    }
}

void SessionManager::ForceIdle()
{
    End(MediaSourceKind::Unknown);
}

SessionState SessionManager::State() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

MediaSourceKind SessionManager::ActiveSource() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return activeSource_;
}

std::wstring SessionManager::ActiveRemoteName() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return remoteName_;
}

SessionState SessionManager::StateForSource(MediaSourceKind source)
{
    switch (source) {
    case MediaSourceKind::Usb:
        return SessionState::UsbActive;
    case MediaSourceKind::AirPlay:
        return SessionState::AirPlayActive;
    case MediaSourceKind::HidExperimental:
        return SessionState::HidExperimentalActive;
    default:
        return SessionState::Idle;
    }
}
