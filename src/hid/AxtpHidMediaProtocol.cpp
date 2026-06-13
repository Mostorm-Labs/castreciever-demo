#include "hid/AxtpHidMediaProtocol.h"

#include <memory>
#include <utility>

#include "core/inbound/frame_decoder.hpp"
#include "core/inbound/message_reassembler.hpp"
#include "core/inbound/payload_decoder.hpp"
#include "model/payload.hpp"

struct AxtpHidMediaProtocol::Impl final : public axtp::IPayloadSink {
    explicit Impl(IAxtpHidMediaProtocolSink& sink)
        : sink(sink)
    {
        Reset();
    }

    void Reset()
    {
        payloadDecoder = std::make_unique<axtp::PayloadDecoder>(*this);
        messageReassembler = std::make_unique<axtp::MessageReassembler>(*payloadDecoder);
        frameDecoder = std::make_unique<axtp::FrameDecoder>(*messageReassembler);
    }

    void onControl(axtp::ControlPayload payload) override
    {
        sink.OnAxtpControlPayload(payload.controlId, std::move(payload.body));
    }

    void onRpc(axtp::RpcPayload payload) override
    {
        sink.OnAxtpRpcPayload(payload.requestId, payload.methodOrEventId, std::move(payload.body));
    }

    void onStream(axtp::StreamPayload payload) override
    {
        sink.OnAxtpStreamPayload(
            payload.streamId,
            payload.seqId,
            payload.cursor,
            std::move(payload.data));
    }

    void SubmitBytes(const uint8_t* data, size_t byteCount)
    {
        if (data == nullptr || byteCount == 0 || !frameDecoder) {
            return;
        }
        frameDecoder->onBytes(data, byteCount);
    }

    IAxtpHidMediaProtocolSink& sink;
    uint8_t reportId = 0;
    std::unique_ptr<axtp::PayloadDecoder> payloadDecoder;
    std::unique_ptr<axtp::MessageReassembler> messageReassembler;
    std::unique_ptr<axtp::FrameDecoder> frameDecoder;
};

AxtpHidMediaProtocol::AxtpHidMediaProtocol(IAxtpHidMediaProtocolSink& sink)
    : impl_(new Impl(sink))
{
}

AxtpHidMediaProtocol::~AxtpHidMediaProtocol()
{
    delete impl_;
}

void AxtpHidMediaProtocol::Reset()
{
    impl_->Reset();
}

void AxtpHidMediaProtocol::SetReportId(uint8_t reportId)
{
    impl_->reportId = reportId;
}

void AxtpHidMediaProtocol::SubmitHidReport(const uint8_t* data, size_t byteCount)
{
    if (data == nullptr || byteCount == 0) {
        return;
    }

    if (impl_->reportId != 0) {
        if (data[0] != impl_->reportId) {
            return;
        }
        impl_->SubmitBytes(data + 1, byteCount - 1);
        return;
    }

    impl_->SubmitBytes(data, byteCount);
}

void AxtpHidMediaProtocol::SubmitAxtpBytes(const uint8_t* data, size_t byteCount)
{
    impl_->SubmitBytes(data, byteCount);
}
