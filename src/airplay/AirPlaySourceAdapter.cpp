#include "airplay/AirPlaySourceAdapter.h"

#include "Log.h"

HRESULT AirPlaySourceAdapter::Start(const SourceStartContext&)
{
    Log::Write(L"AirPlaySourceAdapter scaffold is present; RAOP/HTTP/FairPlay migration is the next integration phase.");
    return HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED);
}

void AirPlaySourceAdapter::Stop()
{
}

void AirPlaySourceAdapter::Pause()
{
}

void AirPlaySourceAdapter::Resume()
{
}

