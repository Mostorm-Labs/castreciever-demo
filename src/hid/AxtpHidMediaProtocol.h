#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace axtp {
struct ControlPayload;
struct RpcPayload;
struct StreamPayload;
} // namespace axtp

class IAxtpHidMediaProtocolSink {
public:
    virtual ~IAxtpHidMediaProtocolSink() = default;
    virtual void OnAxtpStreamPayload(uint32_t streamId, uint32_t seqId, uint64_t cursor, std::vector<uint8_t> data) = 0;
    virtual void OnAxtpControlPayload(uint16_t controlId, std::vector<uint8_t> payload) = 0;
    virtual void OnAxtpRpcPayload(uint32_t requestId, uint32_t methodOrEventId, std::vector<uint8_t> payload) = 0;
    virtual void OnAxtpProtocolWarning(const std::wstring& message) = 0;
};

class AxtpHidMediaProtocol {
public:
    explicit AxtpHidMediaProtocol(IAxtpHidMediaProtocolSink& sink);
    ~AxtpHidMediaProtocol();

    void Reset();
    void SetReportId(uint8_t reportId);
    void SubmitHidReport(const uint8_t* data, size_t byteCount);
    void SubmitAxtpBytes(const uint8_t* data, size_t byteCount);

private:
    struct Impl;

    Impl* impl_ = nullptr;
};
