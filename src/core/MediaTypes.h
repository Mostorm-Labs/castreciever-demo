#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class MediaSourceKind {
    Unknown,
    Usb,
    AirPlay,
    HidExperimental,
    SelfTest,
};

enum class MediaTimestampDomain {
    Unknown,
    LocalQpcNs,
    MediaFoundationHns,
    AirPlayNtp,
    HidTicks,
};

enum class MediaEventType {
    Started,
    Stopped,
    Connected,
    Disconnected,
    Busy,
    PinRequired,
    FormatChanged,
    DiscoveryError,
    Error,
};

enum class VideoFramePayloadKind {
    None,
    EncodedAnnexBH264,
    DecodedD3D11Nv12Texture,
    SelfTest,
};

struct VideoFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameRateNumerator = 30;
    uint32_t frameRateDenominator = 1;
};

struct MediaVideoFrame {
    MediaSourceKind source = MediaSourceKind::Unknown;
    VideoFramePayloadKind payloadKind = VideoFramePayloadKind::None;
    VideoFormat format;
    int64_t ptsQpcNs = 0;
    int64_t durationNs = 0;
    uint64_t sequence = 0;
    bool keyFrame = false;
    std::vector<uint8_t> encodedBytes;
    std::shared_ptr<void> nativeHandle;
    uint32_t nativeSubresourceIndex = 0;
};

struct MediaAudioFrame {
    MediaSourceKind source = MediaSourceKind::Unknown;
    int64_t ptsQpcNs = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    std::vector<uint8_t> pcmBytes;
};

struct MediaEvent {
    MediaSourceKind source = MediaSourceKind::Unknown;
    MediaEventType type = MediaEventType::Error;
    std::wstring message;
};

