#pragma once

#include "hid/AxtpHidMediaProtocol.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "io/byte_sink.hpp"

namespace axtp {
class HidTransport;
} // namespace axtp

struct AxtpRuntimeHidConfig {
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

class AxtpRuntimeHidAdapter final : public axtp::IByteSink {
public:
    explicit AxtpRuntimeHidAdapter(AxtpHidMediaProtocol& protocol);
    ~AxtpRuntimeHidAdapter() override;

    HRESULT Start(const AxtpRuntimeHidConfig& config);
    void Stop();
    bool Running() const { return running_.load(); }

    void onBytes(const axtp::Byte* data, std::size_t size) override;

private:
    void PollLoop();

    AxtpHidMediaProtocol& protocol_;
    AxtpRuntimeHidConfig config_;
    std::unique_ptr<axtp::HidTransport> transport_;
    std::thread pollThread_;
    std::atomic_bool running_ { false };
};
