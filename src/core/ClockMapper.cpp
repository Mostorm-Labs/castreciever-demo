#include "core/ClockMapper.h"

#include <windows.h>

#include <algorithm>

namespace {

int64_t ScaleUnsignedDeltaToNs(uint64_t delta, uint64_t unitsPerSecond)
{
    if (unitsPerSecond == 0) {
        return 0;
    }

    const long double ns = (static_cast<long double>(delta) * 1000000000.0L) /
        static_cast<long double>(unitsPerSecond);
    return static_cast<int64_t>(ns);
}

} // namespace

ClockMapper::ClockMapper()
{
    Reset();
}

int64_t ClockMapper::NowQpcNs()
{
    LARGE_INTEGER counter = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    if (frequency.QuadPart <= 0) {
        return 0;
    }

    const long double ns = (static_cast<long double>(counter.QuadPart) * 1000000000.0L) /
        static_cast<long double>(frequency.QuadPart);
    return static_cast<int64_t>(ns);
}

void ClockMapper::Reset()
{
    domain_ = MediaTimestampDomain::LocalQpcNs;
    sourceBase_ = NowQpcNs();
    localBaseQpcNs_ = sourceBase_;
    hidTicksPerSecond_ = 0;
}

void ClockMapper::ResetMediaFoundation(int64_t firstTimestampHns, int64_t localQpcNs)
{
    domain_ = MediaTimestampDomain::MediaFoundationHns;
    sourceBase_ = firstTimestampHns;
    localBaseQpcNs_ = localQpcNs;
    hidTicksPerSecond_ = 0;
}

void ClockMapper::ResetAirPlayNtp(uint64_t firstNtpTimestamp, int64_t localQpcNs)
{
    domain_ = MediaTimestampDomain::AirPlayNtp;
    sourceBase_ = NtpToNsSinceEra(firstNtpTimestamp);
    localBaseQpcNs_ = localQpcNs;
    hidTicksPerSecond_ = 0;
}

void ClockMapper::ResetHidTicks(uint64_t firstTick, uint64_t ticksPerSecond, int64_t localQpcNs)
{
    domain_ = MediaTimestampDomain::HidTicks;
    sourceBase_ = static_cast<int64_t>(firstTick);
    localBaseQpcNs_ = localQpcNs;
    hidTicksPerSecond_ = ticksPerSecond;
}

int64_t ClockMapper::MapLocalQpcNs(int64_t qpcNs) const
{
    return qpcNs;
}

int64_t ClockMapper::MapMediaFoundationHns(int64_t timestampHns) const
{
    return localBaseQpcNs_ + ((timestampHns - sourceBase_) * 100);
}

int64_t ClockMapper::MapAirPlayNtp(uint64_t ntpTimestamp) const
{
    return localBaseQpcNs_ + (NtpToNsSinceEra(ntpTimestamp) - sourceBase_);
}

int64_t ClockMapper::MapHidTicks(uint64_t tick) const
{
    const uint64_t base = sourceBase_ < 0 ? 0 : static_cast<uint64_t>(sourceBase_);
    if (tick >= base) {
        return localBaseQpcNs_ + ScaleUnsignedDeltaToNs(tick - base, hidTicksPerSecond_);
    }
    return localBaseQpcNs_ - ScaleUnsignedDeltaToNs(base - tick, hidTicksPerSecond_);
}

void ClockMapper::RebaseTo(int64_t sourceTimestamp, int64_t localQpcNs, MediaTimestampDomain domain)
{
    domain_ = domain;
    sourceBase_ = sourceTimestamp;
    localBaseQpcNs_ = localQpcNs;
}

int64_t ClockMapper::NtpToNsSinceEra(uint64_t ntpTimestamp)
{
    const uint64_t seconds = ntpTimestamp >> 32;
    const uint64_t fraction = ntpTimestamp & 0xFFFFFFFFULL;
    const uint64_t fractionalNs = (fraction * 1000000000ULL) >> 32;
    return static_cast<int64_t>(seconds * 1000000000ULL + fractionalNs);
}
