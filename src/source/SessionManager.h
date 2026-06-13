#pragma once

#include "core/MediaTypes.h"

#include <mutex>
#include <string>

enum class SessionState {
    Idle,
    UsbActive,
    AirPlayActive,
    HidExperimentalActive,
};

class SessionManager {
public:
    bool TryBegin(MediaSourceKind source, std::wstring remoteName = {});
    void End(MediaSourceKind source);
    void ForceIdle();

    SessionState State() const;
    MediaSourceKind ActiveSource() const;
    std::wstring ActiveRemoteName() const;

private:
    static SessionState StateForSource(MediaSourceKind source);

    mutable std::mutex mutex_;
    SessionState state_ = SessionState::Idle;
    MediaSourceKind activeSource_ = MediaSourceKind::Unknown;
    std::wstring remoteName_;
};

