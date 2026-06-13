#pragma once

#include "source/ISourceAdapter.h"

class UsbUvcSourceAdapter final : public ISourceAdapter {
public:
    MediaSourceKind SourceKind() const override { return MediaSourceKind::Usb; }
    HRESULT Start(const SourceStartContext& context) override;
    void Stop() override;
    void Pause() override;
    void Resume() override;
};

