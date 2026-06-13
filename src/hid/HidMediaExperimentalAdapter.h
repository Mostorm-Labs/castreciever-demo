#pragma once

#include "core/ClockMapper.h"
#include "hid/AxtpHidMediaProtocol.h"
#include "source/ISourceAdapter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class AxtpRuntimeHidAdapter;

enum class HidMediaStreamKind {
    Video,
    Audio,
};

struct HidMediaStreamConfig {
    uint32_t streamId = 0;
    HidMediaStreamKind kind = HidMediaStreamKind::Video;
    uint32_t maxDataSize = 1024 * 1024;
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
};

struct HidRuntimeTransportConfig {
    bool enabled = false;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    std::string serialNumber;
    uint8_t reportId = 0;
    size_t inputReportSize = 64;
    size_t outputReportSize = 64;
    size_t maxReportsPerPoll = 16;
    uint32_t pollIntervalMs = 1;
};

class HidMediaExperimentalAdapter final : public ISourceAdapter, public IAxtpHidMediaProtocolSink {
public:
    HidMediaExperimentalAdapter();
    ~HidMediaExperimentalAdapter() override;

    MediaSourceKind SourceKind() const override { return MediaSourceKind::HidExperimental; }
    HRESULT Start(const SourceStartContext& context) override;
    void Stop() override;
    void Pause() override;
    void Resume() override;

    HRESULT SubmitReportForTest(const uint8_t* data, size_t byteCount);
    HRESULT SubmitAxtpBytesForTest(const uint8_t* data, size_t byteCount);
    void ConfigureStreamForTest(const HidMediaStreamConfig& config);
    void SetRuntimeTransportConfig(const HidRuntimeTransportConfig& config);

    void OnAxtpStreamPayload(uint32_t streamId, uint32_t seqId, uint64_t cursor, std::vector<uint8_t> data) override;
    void OnAxtpControlPayload(uint16_t controlId, std::vector<uint8_t> payload) override;
    void OnAxtpRpcPayload(uint32_t requestId, uint32_t methodOrEventId, std::vector<uint8_t> payload) override;
    void OnAxtpProtocolWarning(const std::wstring& message) override;

private:
    struct StreamContext {
        HidMediaStreamKind kind = HidMediaStreamKind::Video;
        uint32_t streamId = 0;
        uint32_t expectedSeqId = 0;
        bool hasExpectedSeqId = false;
        uint32_t maxDataSize = 1024 * 1024;
        uint32_t sampleRate = 48000;
        uint16_t channels = 2;
        uint64_t receivedChunks = 0;
        uint64_t missingChunks = 0;
        uint64_t duplicateOrOldChunks = 0;
    };

    struct VideoReassemblyState {
        uint32_t frameId = 0;
        uint32_t frameLength = 0;
        uint64_t timestampUs = 0;
        uint64_t firstSeqId = 0;
        uint32_t receivedByteCount = 0;
        bool keyFrame = false;
        std::vector<uint8_t> bytes;
        std::vector<bool> received;
        std::chrono::steady_clock::time_point startedAt;
    };

    struct ParsedStreamPayload {
        uint32_t streamId = 0;
        uint32_t seqId = 0;
        uint64_t cursor = 0;
        std::vector<uint8_t> data;
    };

    struct VideoChunkEnvelope {
        bool hasEnvelope = false;
        uint16_t flags = 0;
        uint32_t frameId = 0;
        uint32_t frameOffset = 0;
        uint32_t frameLength = 0;
        uint64_t timestampUs = 0;
        uint64_t receiverTimestampUs = 0;
        std::vector<uint8_t> payloadBytes;
    };

    struct AudioChunkEnvelope {
        bool hasEnvelope = false;
        uint16_t flags = 0;
        uint64_t timestampUs = 0;
        uint64_t receiverTimestampUs = 0;
        uint32_t sampleCount = 0;
        uint32_t durationUs = 0;
        std::vector<uint8_t> payloadBytes;
    };

    void ConfigureDefaultStreams();
    void ResetVideoReassembly();
    bool VideoReassemblyTimedOut() const;
    void HandleStreamPayload(ParsedStreamPayload payload);
    bool ValidateSeq(StreamContext& context, uint32_t seqId);
    void HandleVideoPayload(StreamContext& context, const ParsedStreamPayload& payload);
    void HandleAudioPayload(StreamContext& context, const ParsedStreamPayload& payload);
    VideoChunkEnvelope ParseVideoChunkEnvelope(const ParsedStreamPayload& payload) const;
    AudioChunkEnvelope ParseAudioChunkEnvelope(const ParsedStreamPayload& payload) const;
    int64_t MapTimestampUsToQpcNs(uint64_t timestampUs);
    void SubmitVideoFrame(const VideoChunkEnvelope& envelope, uint64_t sequence);
    void SubmitAudioFrame(const AudioChunkEnvelope& envelope, const StreamContext& context, uint64_t sequence);

    IMediaSink* mediaSink_ = nullptr;
    bool running_ = false;
    bool paused_ = false;
    bool clockReady_ = false;
    ClockMapper clockMapper_;
    AxtpHidMediaProtocol protocol_;
    std::unique_ptr<AxtpRuntimeHidAdapter> runtimeHidAdapter_;
    HidRuntimeTransportConfig runtimeTransportConfig_;
    std::map<uint32_t, StreamContext> streams_;
    VideoReassemblyState videoReassembly_;
};
