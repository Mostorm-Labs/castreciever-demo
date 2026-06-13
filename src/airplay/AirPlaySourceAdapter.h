#pragma once

#include "source/ISourceAdapter.h"

class AirPlaySourceAdapter final : public ISourceAdapter {
public:
    MediaSourceKind SourceKind() const override { return MediaSourceKind::AirPlay; }
    HRESULT Start(const SourceStartContext& context) override;
    void Stop() override;
    void Pause() override;
    void Resume() override;
};

