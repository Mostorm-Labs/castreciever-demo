#include "hid/HidMediaExperimentalAdapter.h"

#include "Log.h"
#include "core/ClockMapper.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

constexpr size_t kHeaderSize = 12;
constexpr uint32_t kMaxFrameBytes = 4 * 1024 * 1024;
constexpr auto kFragmentTimeout = std::chrono::milliseconds(100);

uint16_t ReadLe16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t ReadLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

bool AllFragmentsReceived(const std::vector<bool>& received)
{
    return std::all_of(received.begin(), received.end(), [](bool value) { return value; });
}

} // namespace

HRESULT HidMediaExperimentalAdapter::Start(const SourceStartContext& context)
{
    mediaSink_ = context.mediaSink;
    running_ = true;
    ResetReassembly();
    Log::Write(L"HID media experimental adapter started in scaffold mode.");
    return S_OK;
}

void HidMediaExperimentalAdapter::Stop()
{
    running_ = false;
    mediaSink_ = nullptr;
    ResetReassembly();
}

void HidMediaExperimentalAdapter::Pause()
{
}

void HidMediaExperimentalAdapter::Resume()
{
}

HRESULT HidMediaExperimentalAdapter::SubmitReportForTest(const uint8_t* data, size_t byteCount)
{
    if (!running_ || mediaSink_ == nullptr) {
        return E_UNEXPECTED;
    }
    if (data == nullptr || byteCount < kHeaderSize) {
        return E_INVALIDARG;
    }
    if (ReassemblyTimedOut()) {
        Log::Write(L"HID experimental frame reassembly timed out; dropping partial frame id=%u.", reassembly_.frameId);
        ResetReassembly();
    }

    const uint32_t frameId = ReadLe32(data);
    const uint16_t fragmentIndex = ReadLe16(data + 4);
    const uint16_t fragmentCount = ReadLe16(data + 6);
    const uint32_t totalSize = ReadLe32(data + 8);
    const size_t payloadSize = byteCount - kHeaderSize;

    if (fragmentCount == 0 || fragmentIndex >= fragmentCount || totalSize == 0 || totalSize > kMaxFrameBytes) {
        ResetReassembly();
        return E_INVALIDARG;
    }

    if (reassembly_.bytes.empty() || reassembly_.frameId != frameId) {
        reassembly_.frameId = frameId;
        reassembly_.fragmentCount = fragmentCount;
        reassembly_.totalSize = totalSize;
        reassembly_.bytes.assign(totalSize, 0);
        reassembly_.received.assign(fragmentCount, false);
        reassembly_.startedAt = std::chrono::steady_clock::now();
    }

    if (reassembly_.fragmentCount != fragmentCount || reassembly_.totalSize != totalSize) {
        ResetReassembly();
        return E_INVALIDARG;
    }

    const size_t offset = (static_cast<size_t>(totalSize) * fragmentIndex) / fragmentCount;
    const size_t nextOffset = (static_cast<size_t>(totalSize) * (fragmentIndex + 1)) / fragmentCount;
    const size_t expectedPayloadSize = nextOffset - offset;
    if (payloadSize != expectedPayloadSize || offset + payloadSize > reassembly_.bytes.size()) {
        ResetReassembly();
        return E_INVALIDARG;
    }

    std::memcpy(reassembly_.bytes.data() + offset, data + kHeaderSize, payloadSize);
    reassembly_.received[fragmentIndex] = true;

    if (!AllFragmentsReceived(reassembly_.received)) {
        return S_FALSE;
    }

    MediaVideoFrame frame;
    frame.source = MediaSourceKind::HidExperimental;
    frame.payloadKind = VideoFramePayloadKind::EncodedAnnexBH264;
    frame.ptsQpcNs = ClockMapper::NowQpcNs();
    frame.sequence = frameId;
    frame.encodedBytes = std::move(reassembly_.bytes);
    ResetReassembly();
    return mediaSink_->SubmitVideo(std::move(frame));
}

void HidMediaExperimentalAdapter::ResetReassembly()
{
    reassembly_ = {};
}

bool HidMediaExperimentalAdapter::ReassemblyTimedOut() const
{
    if (reassembly_.bytes.empty()) {
        return false;
    }

    return std::chrono::steady_clock::now() - reassembly_.startedAt > kFragmentTimeout;
}
