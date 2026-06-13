#pragma once

#include "source/ISourceAdapter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

class HidMediaExperimentalAdapter final : public ISourceAdapter {
public:
    MediaSourceKind SourceKind() const override { return MediaSourceKind::HidExperimental; }
    HRESULT Start(const SourceStartContext& context) override;
    void Stop() override;
    void Pause() override;
    void Resume() override;

    HRESULT SubmitReportForTest(const uint8_t* data, size_t byteCount);

private:
    struct ReassemblyState {
        uint32_t frameId = 0;
        uint16_t fragmentCount = 0;
        uint32_t totalSize = 0;
        std::vector<uint8_t> bytes;
        std::vector<bool> received;
        std::chrono::steady_clock::time_point startedAt;
    };

    void ResetReassembly();
    bool ReassemblyTimedOut() const;

    IMediaSink* mediaSink_ = nullptr;
    bool running_ = false;
    ReassemblyState reassembly_;
};
