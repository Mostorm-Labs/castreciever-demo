#include "source/UsbUvcSourceAdapter.h"

#include "Log.h"

HRESULT UsbUvcSourceAdapter::Start(const SourceStartContext&)
{
    Log::Write(L"UsbUvcSourceAdapter scaffold is present; current USB path still uses SourceReaderD3D11Player directly.");
    return S_OK;
}

void UsbUvcSourceAdapter::Stop()
{
}

void UsbUvcSourceAdapter::Pause()
{
}

void UsbUvcSourceAdapter::Resume()
{
}

