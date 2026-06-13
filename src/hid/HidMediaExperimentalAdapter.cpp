#include "hid/HidMediaExperimentalAdapter.h"

#include "HResult.h"
#include "Log.h"
#include "hid/AxtpRuntimeHidAdapter.h"

#include <utility>

namespace {

constexpr uint32_t kDefaultVideoStreamId = 1;
constexpr uint32_t kDefaultAudioStreamId = 2;
constexpr uint32_t kMaxVideoFrameBytes = 8 * 1024 * 1024;
constexpr uint32_t kMaxAudioChunkBytes = 512 * 1024;
constexpr auto kVideoReassemblyTimeout = std::chrono::milliseconds(100);

enum VideoChunkFlags : uint16_t {
    kVideoFrameStart = 1u << 0,
    kVideoFrameEnd = 1u << 1,
    kVideoKeyFrame = 1u << 2,
    kVideoConfig = 1u << 3,
};

enum AudioChunkFlags : uint16_t {
    kAudioAccessUnitStart = 1u << 0,
    kAudioAccessUnitEnd = 1u << 1,
    kAudioConfig = 1u << 2,
    kAudioDiscontinuity = 1u << 3,
};

constexpr uint16_t kKnownVideoChunkFlags =
    kVideoFrameStart | kVideoFrameEnd | kVideoKeyFrame | kVideoConfig;
constexpr uint16_t kKnownAudioChunkFlags =
    kAudioAccessUnitStart | kAudioAccessUnitEnd | kAudioConfig | kAudioDiscontinuity;

uint16_t ReadU16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t ReadU32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

uint64_t ReadU64(const uint8_t* data)
{
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool IsSeqOlderOrDuplicate(uint32_t seqId, uint32_t expectedSeqId)
{
    return static_cast<int32_t>(seqId - expectedSeqId) < 0;
}

} // namespace

HidMediaExperimentalAdapter::HidMediaExperimentalAdapter()
    : protocol_(*this)
    , runtimeHidAdapter_(std::make_unique<AxtpRuntimeHidAdapter>(protocol_))
{
}

HidMediaExperimentalAdapter::~HidMediaExperimentalAdapter()
{
    Stop();
}

HRESULT HidMediaExperimentalAdapter::Start(const SourceStartContext& context)
{
    mediaSink_ = context.mediaSink;
    running_ = true;
    paused_ = false;
    clockReady_ = false;
    protocol_.Reset();
    protocol_.SetReportId(runtimeTransportConfig_.reportId);
    ConfigureDefaultStreams();
    ResetVideoReassembly();

    if (runtimeTransportConfig_.enabled) {
        HRESULT hr = runtimeHidAdapter_->Start({
            runtimeTransportConfig_.enabled,
            runtimeTransportConfig_.vendorId,
            runtimeTransportConfig_.productId,
            runtimeTransportConfig_.serialNumber,
            runtimeTransportConfig_.reportId,
            runtimeTransportConfig_.inputReportSize,
            runtimeTransportConfig_.outputReportSize,
            runtimeTransportConfig_.maxReportsPerPoll,
            runtimeTransportConfig_.pollIntervalMs,
        });
        if (FAILED(hr)) {
            running_ = false;
            paused_ = false;
            clockReady_ = false;
            mediaSink_ = nullptr;
            streams_.clear();
            protocol_.Reset();
            ResetVideoReassembly();
            return hr;
        }
    }

    if (mediaSink_ != nullptr) {
        mediaSink_->NotifyEvent(MediaEvent {
            MediaSourceKind::HidExperimental,
            MediaEventType::Started,
            L"HID AXTP media adapter started.",
        });
    }

    Log::Write(L"HID media experimental adapter started with AXTP STREAM defaults: video streamId=%u, audio streamId=%u.",
        kDefaultVideoStreamId,
        kDefaultAudioStreamId);
    return S_OK;
}

void HidMediaExperimentalAdapter::Stop()
{
    if (runtimeHidAdapter_) {
        runtimeHidAdapter_->Stop();
    }
    running_ = false;
    paused_ = false;
    mediaSink_ = nullptr;
    streams_.clear();
    protocol_.Reset();
    ResetVideoReassembly();
}

void HidMediaExperimentalAdapter::Pause()
{
    paused_ = true;
}

void HidMediaExperimentalAdapter::Resume()
{
    paused_ = false;
}

HRESULT HidMediaExperimentalAdapter::SubmitReportForTest(const uint8_t* data, size_t byteCount)
{
    if (!running_) {
        return E_UNEXPECTED;
    }
    if (data == nullptr || byteCount == 0) {
        return E_INVALIDARG;
    }

    protocol_.SubmitHidReport(data, byteCount);
    return S_OK;
}

HRESULT HidMediaExperimentalAdapter::SubmitAxtpBytesForTest(const uint8_t* data, size_t byteCount)
{
    if (!running_) {
        return E_UNEXPECTED;
    }
    if (data == nullptr || byteCount == 0) {
        return E_INVALIDARG;
    }

    protocol_.SubmitAxtpBytes(data, byteCount);
    return S_OK;
}

void HidMediaExperimentalAdapter::ConfigureStreamForTest(const HidMediaStreamConfig& config)
{
    if (config.streamId == 0) {
        return;
    }

    StreamContext context;
    context.kind = config.kind;
    context.streamId = config.streamId;
    context.maxDataSize = config.maxDataSize;
    context.sampleRate = config.sampleRate;
    context.channels = config.channels;
    streams_[config.streamId] = context;
}

void HidMediaExperimentalAdapter::SetRuntimeTransportConfig(const HidRuntimeTransportConfig& config)
{
    runtimeTransportConfig_ = config;
}

void HidMediaExperimentalAdapter::OnAxtpStreamPayload(uint32_t streamId, uint32_t seqId, uint64_t cursor, std::vector<uint8_t> data)
{
    if (!running_ || paused_) {
        return;
    }

    ParsedStreamPayload parsed;
    parsed.streamId = streamId;
    parsed.seqId = seqId;
    parsed.cursor = cursor;
    parsed.data = std::move(data);
    HandleStreamPayload(std::move(parsed));
}

void HidMediaExperimentalAdapter::OnAxtpControlPayload(uint16_t controlId, std::vector<uint8_t>)
{
    Log::Write(L"HID AXTP CONTROL payload ignored by media adapter: controlId=%u.", controlId);
}

void HidMediaExperimentalAdapter::OnAxtpRpcPayload(uint32_t requestId, uint32_t methodOrEventId, std::vector<uint8_t>)
{
    Log::Write(L"HID AXTP RPC payload ignored by media adapter: requestId=%u methodOrEventId=%u.",
        requestId,
        methodOrEventId);
}

void HidMediaExperimentalAdapter::OnAxtpProtocolWarning(const std::wstring& message)
{
    Log::Write(L"HID AXTP parser warning: %s", message.c_str());
}

void HidMediaExperimentalAdapter::ConfigureDefaultStreams()
{
    streams_.clear();

    HidMediaStreamConfig video;
    video.streamId = kDefaultVideoStreamId;
    video.kind = HidMediaStreamKind::Video;
    video.maxDataSize = 2 * 1024 * 1024;
    ConfigureStreamForTest(video);

    HidMediaStreamConfig audio;
    audio.streamId = kDefaultAudioStreamId;
    audio.kind = HidMediaStreamKind::Audio;
    audio.maxDataSize = kMaxAudioChunkBytes;
    audio.sampleRate = 48000;
    audio.channels = 2;
    ConfigureStreamForTest(audio);
}

void HidMediaExperimentalAdapter::ResetVideoReassembly()
{
    videoReassembly_ = {};
}

bool HidMediaExperimentalAdapter::VideoReassemblyTimedOut() const
{
    if (videoReassembly_.bytes.empty()) {
        return false;
    }

    return std::chrono::steady_clock::now() - videoReassembly_.startedAt > kVideoReassemblyTimeout;
}

void HidMediaExperimentalAdapter::HandleStreamPayload(ParsedStreamPayload payload)
{
    auto it = streams_.find(payload.streamId);
    if (it == streams_.end()) {
        Log::Write(L"HID AXTP STREAM unknown streamId=%u.", payload.streamId);
        return;
    }

    StreamContext& context = it->second;
    if (payload.data.size() > context.maxDataSize) {
        Log::Write(L"HID AXTP STREAM payload too large: streamId=%u bytes=%zu max=%u.",
            payload.streamId,
            payload.data.size(),
            context.maxDataSize);
        return;
    }

    if (!ValidateSeq(context, payload.seqId)) {
        return;
    }
    ++context.receivedChunks;

    if (context.kind == HidMediaStreamKind::Video) {
        HandleVideoPayload(context, payload);
    } else {
        HandleAudioPayload(context, payload);
    }
}

bool HidMediaExperimentalAdapter::ValidateSeq(StreamContext& context, uint32_t seqId)
{
    if (!context.hasExpectedSeqId) {
        context.expectedSeqId = seqId + 1;
        context.hasExpectedSeqId = true;
        return true;
    }

    if (seqId == context.expectedSeqId) {
        ++context.expectedSeqId;
        return true;
    }

    if (IsSeqOlderOrDuplicate(seqId, context.expectedSeqId)) {
        ++context.duplicateOrOldChunks;
        Log::Write(L"HID AXTP STREAM duplicate/old seqId: streamId=%u seq=%u expected=%u.",
            context.streamId,
            seqId,
            context.expectedSeqId);
        return false;
    }

    const uint32_t missing = seqId - context.expectedSeqId;
    context.missingChunks += missing;
    context.expectedSeqId = seqId + 1;
    Log::Write(L"HID AXTP STREAM seq gap: streamId=%u seq=%u missing=%u.",
        context.streamId,
        seqId,
        missing);

    if (context.kind == HidMediaStreamKind::Video) {
        ResetVideoReassembly();
    }
    return true;
}

void HidMediaExperimentalAdapter::HandleVideoPayload(StreamContext&, const ParsedStreamPayload& payload)
{
    if (VideoReassemblyTimedOut()) {
        Log::Write(L"HID AXTP video frame reassembly timed out; dropping frame id=%u.",
            videoReassembly_.frameId);
        ResetVideoReassembly();
    }

    VideoChunkEnvelope envelope = ParseVideoChunkEnvelope(payload);
    if (!envelope.hasEnvelope) {
        envelope.flags = kVideoFrameStart | kVideoFrameEnd;
        envelope.frameId = static_cast<uint32_t>(payload.seqId);
        envelope.frameOffset = 0;
        envelope.frameLength = static_cast<uint32_t>(payload.data.size());
        envelope.timestampUs = payload.cursor;
        envelope.payloadBytes = payload.data;
    }

    if (envelope.payloadBytes.empty()) {
        return;
    }

    if (envelope.frameLength == 0) {
        envelope.frameLength = envelope.frameOffset + static_cast<uint32_t>(envelope.payloadBytes.size());
    }
    if (envelope.frameLength > kMaxVideoFrameBytes ||
        envelope.frameOffset > envelope.frameLength ||
        envelope.frameOffset + envelope.payloadBytes.size() > envelope.frameLength) {
        Log::Write(L"HID AXTP video chunk invalid: frameId=%u offset=%u payload=%zu frameLength=%u.",
            envelope.frameId,
            envelope.frameOffset,
            envelope.payloadBytes.size(),
            envelope.frameLength);
        ResetVideoReassembly();
        return;
    }

    const bool startsFrame = (envelope.flags & kVideoFrameStart) != 0 || envelope.frameOffset == 0;
    if (startsFrame || videoReassembly_.bytes.empty() || videoReassembly_.frameId != envelope.frameId) {
        videoReassembly_.frameId = envelope.frameId;
        videoReassembly_.frameLength = envelope.frameLength;
        videoReassembly_.timestampUs = envelope.timestampUs;
        videoReassembly_.firstSeqId = payload.seqId;
        videoReassembly_.receivedByteCount = 0;
        videoReassembly_.keyFrame = (envelope.flags & (kVideoKeyFrame | kVideoConfig)) != 0;
        videoReassembly_.bytes.assign(envelope.frameLength, 0);
        videoReassembly_.received.assign(envelope.frameLength, false);
        videoReassembly_.startedAt = std::chrono::steady_clock::now();
    }

    if (videoReassembly_.frameLength != envelope.frameLength) {
        Log::Write(L"HID AXTP video frame length changed mid-frame; dropping frame id=%u.", envelope.frameId);
        ResetVideoReassembly();
        return;
    }

    const size_t offset = envelope.frameOffset;
    for (size_t index = 0; index < envelope.payloadBytes.size(); ++index) {
        const size_t target = offset + index;
        videoReassembly_.bytes[target] = envelope.payloadBytes[index];
        if (!videoReassembly_.received[target]) {
            videoReassembly_.received[target] = true;
            ++videoReassembly_.receivedByteCount;
        }
    }
    videoReassembly_.keyFrame = videoReassembly_.keyFrame || (envelope.flags & (kVideoKeyFrame | kVideoConfig)) != 0;

    const bool hasWholeFrame = videoReassembly_.receivedByteCount == videoReassembly_.frameLength;
    const bool endsFrame = (envelope.flags & kVideoFrameEnd) != 0;
    if (!endsFrame && !hasWholeFrame) {
        return;
    }
    if (!hasWholeFrame) {
        Log::Write(L"HID AXTP video frame ended with missing bytes; dropping frame id=%u.", envelope.frameId);
        ResetVideoReassembly();
        return;
    }

    envelope.payloadBytes = std::move(videoReassembly_.bytes);
    envelope.frameOffset = 0;
    envelope.frameLength = static_cast<uint32_t>(envelope.payloadBytes.size());
    envelope.timestampUs = videoReassembly_.timestampUs;
    envelope.flags |= videoReassembly_.keyFrame ? kVideoKeyFrame : 0;
    const uint64_t sequence = videoReassembly_.firstSeqId;
    ResetVideoReassembly();
    SubmitVideoFrame(envelope, sequence);
}

void HidMediaExperimentalAdapter::HandleAudioPayload(StreamContext& context, const ParsedStreamPayload& payload)
{
    AudioChunkEnvelope envelope = ParseAudioChunkEnvelope(payload);
    if (!envelope.hasEnvelope) {
        envelope.flags = kAudioAccessUnitStart | kAudioAccessUnitEnd;
        envelope.timestampUs = payload.cursor;
        envelope.payloadBytes = payload.data;
    }

    if (envelope.payloadBytes.empty()) {
        return;
    }

    SubmitAudioFrame(envelope, context, payload.seqId);
}

HidMediaExperimentalAdapter::VideoChunkEnvelope HidMediaExperimentalAdapter::ParseVideoChunkEnvelope(
    const ParsedStreamPayload& payload) const
{
    // Provisional draft layout; replace with generated AXTP media envelope once adopted.
    VideoChunkEnvelope envelope;
    if (payload.data.size() < 24) {
        return envelope;
    }

    const uint8_t* data = payload.data.data();
    const uint16_t headerLength = ReadU16(data);
    if (headerLength < 24 || headerLength > payload.data.size()) {
        return envelope;
    }

    envelope.hasEnvelope = true;
    envelope.flags = ReadU16(data + 2);
    if ((envelope.flags & ~kKnownVideoChunkFlags) != 0) {
        return {};
    }
    envelope.frameId = ReadU32(data + 4);
    envelope.frameOffset = ReadU32(data + 8);
    envelope.frameLength = ReadU32(data + 12);
    envelope.timestampUs = ReadU64(data + 16);
    if (headerLength >= 32 && payload.data.size() >= 32) {
        envelope.receiverTimestampUs = ReadU64(data + 24);
    }
    envelope.payloadBytes.assign(payload.data.begin() + headerLength, payload.data.end());
    return envelope;
}

HidMediaExperimentalAdapter::AudioChunkEnvelope HidMediaExperimentalAdapter::ParseAudioChunkEnvelope(
    const ParsedStreamPayload& payload) const
{
    // Provisional draft layout; replace with generated AXTP media envelope once adopted.
    AudioChunkEnvelope envelope;
    if (payload.data.size() < 12) {
        return envelope;
    }

    const uint8_t* data = payload.data.data();
    const uint16_t headerLength = ReadU16(data);
    if (headerLength < 12 || headerLength > payload.data.size()) {
        return envelope;
    }

    envelope.hasEnvelope = true;
    envelope.flags = ReadU16(data + 2);
    if ((envelope.flags & ~kKnownAudioChunkFlags) != 0) {
        return {};
    }
    envelope.timestampUs = ReadU64(data + 4);
    if (headerLength >= 20 && payload.data.size() >= 20) {
        envelope.receiverTimestampUs = ReadU64(data + 12);
    }
    if (headerLength >= 24 && payload.data.size() >= 24) {
        envelope.sampleCount = ReadU32(data + 20);
    }
    if (headerLength >= 28 && payload.data.size() >= 28) {
        envelope.durationUs = ReadU32(data + 24);
    }
    envelope.payloadBytes.assign(payload.data.begin() + headerLength, payload.data.end());
    return envelope;
}

int64_t HidMediaExperimentalAdapter::MapTimestampUsToQpcNs(uint64_t timestampUs)
{
    const int64_t now = ClockMapper::NowQpcNs();
    if (!clockReady_) {
        clockMapper_.ResetHidTicks(timestampUs, 1000000, now);
        clockReady_ = true;
    }

    return clockMapper_.MapHidTicks(timestampUs);
}

void HidMediaExperimentalAdapter::SubmitVideoFrame(const VideoChunkEnvelope& envelope, uint64_t sequence)
{
    if (mediaSink_ == nullptr) {
        return;
    }

    MediaVideoFrame frame;
    frame.source = MediaSourceKind::HidExperimental;
    frame.payloadKind = VideoFramePayloadKind::EncodedAnnexBH264;
    frame.ptsQpcNs = MapTimestampUsToQpcNs(envelope.timestampUs);
    frame.mediaTimestampUs = envelope.timestampUs;
    frame.sequence = sequence;
    frame.keyFrame = (envelope.flags & (kVideoKeyFrame | kVideoConfig)) != 0;
    frame.encodedBytes = envelope.payloadBytes;
    LOG_IF_FAILED(mediaSink_->SubmitVideo(std::move(frame)), L"IMediaSink::SubmitVideo(HID AXTP)");
}

void HidMediaExperimentalAdapter::SubmitAudioFrame(
    const AudioChunkEnvelope& envelope,
    const StreamContext& context,
    uint64_t sequence)
{
    if (mediaSink_ == nullptr) {
        return;
    }

    MediaAudioFrame frame;
    frame.source = MediaSourceKind::HidExperimental;
    frame.payloadKind = AudioFramePayloadKind::EncodedAac;
    frame.ptsQpcNs = MapTimestampUsToQpcNs(envelope.timestampUs);
    if (envelope.durationUs != 0) {
        frame.durationNs = static_cast<int64_t>(envelope.durationUs) * 1000;
    } else if (envelope.sampleCount != 0 && context.sampleRate != 0) {
        frame.durationNs = (static_cast<int64_t>(envelope.sampleCount) * 1000000000LL) /
            static_cast<int64_t>(context.sampleRate);
    }
    frame.mediaTimestampUs = envelope.timestampUs;
    frame.sampleRate = context.sampleRate;
    frame.channels = context.channels;
    frame.encodedBytes = envelope.payloadBytes;
    LOG_IF_FAILED(mediaSink_->SubmitAudio(std::move(frame)), L"IMediaSink::SubmitAudio(HID AXTP)");

    if ((sequence % 300) == 0) {
        Log::Write(L"HID AXTP AAC chunk accepted: streamId=%u seq=%llu timestampUs=%llu bytes=%zu.",
            context.streamId,
            sequence,
            envelope.timestampUs,
            envelope.payloadBytes.size());
    }
}
