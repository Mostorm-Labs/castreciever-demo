#include "hid/AxtpRuntimeHidAdapter.h"

#include "HResult.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <exception>

#include "hidapi/hid_transport.hpp"

AxtpRuntimeHidAdapter::AxtpRuntimeHidAdapter(AxtpHidMediaProtocol& protocol)
    : protocol_(protocol)
{
}

AxtpRuntimeHidAdapter::~AxtpRuntimeHidAdapter()
{
    Stop();
}

HRESULT AxtpRuntimeHidAdapter::Start(const AxtpRuntimeHidConfig& config)
{
    Stop();
    config_ = config;
    if (!config_.enabled) {
        return S_FALSE;
    }
    if (config_.inputReportSize < 2 || config_.outputReportSize < 2) {
        Log::Write(L"AXTP runtime HID transport invalid report sizes: input=%zu output=%zu.",
            config_.inputReportSize,
            config_.outputReportSize);
        return E_INVALIDARG;
    }

    axtp::HidTransportOptions options;
    options.vendorId = config_.vendorId;
    options.productId = config_.productId;
    options.serialNumber = config_.serialNumber;
    options.reportId = config_.reportId;
    options.inputReportSize = config_.inputReportSize;
    options.outputReportSize = config_.outputReportSize;
    options.maxReportsPerPoll = config_.maxReportsPerPoll;

    transport_ = std::make_unique<axtp::HidTransport>(options);
    transport_->bind(*this);
    transport_->open();
    if (!transport_->isOpen()) {
        transport_.reset();
        Log::Write(L"AXTP runtime HID transport failed to open: vid=0x%04X pid=0x%04X reportId=%u.",
            config_.vendorId,
            config_.productId,
            config_.reportId);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    running_.store(true);
    pollThread_ = std::thread(&AxtpRuntimeHidAdapter::PollLoop, this);
    Log::Write(L"AXTP runtime HID transport opened: vid=0x%04X pid=0x%04X reportId=%u inputReport=%zu outputReport=%zu.",
        config_.vendorId,
        config_.productId,
        config_.reportId,
        config_.inputReportSize,
        config_.outputReportSize);
    return S_OK;
}

void AxtpRuntimeHidAdapter::Stop()
{
    running_.store(false);
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
}

void AxtpRuntimeHidAdapter::onBytes(const axtp::Byte* data, std::size_t size)
{
    protocol_.SubmitAxtpBytes(data, size);
}

void AxtpRuntimeHidAdapter::PollLoop()
{
    const auto pollInterval = std::chrono::milliseconds(std::max<uint32_t>(1, config_.pollIntervalMs));
    while (running_.load()) {
        try {
            if (transport_) {
                transport_->poll();
            }
        } catch (const std::exception& ex) {
            Log::Write(L"AXTP runtime HID transport poll failed: %S", ex.what());
            running_.store(false);
            break;
        } catch (...) {
            Log::Write(L"AXTP runtime HID transport poll failed with an unknown exception.");
            running_.store(false);
            break;
        }
        std::this_thread::sleep_for(pollInterval);
    }
}
