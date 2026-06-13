#pragma once

#include "core/MediaTypes.h"

#include <cstdint>

class ClockMapper {
public:
    ClockMapper();

    static int64_t NowQpcNs();

    void Reset();
    void ResetMediaFoundation(int64_t firstTimestampHns, int64_t localQpcNs = NowQpcNs());
    void ResetAirPlayNtp(uint64_t firstNtpTimestamp, int64_t localQpcNs = NowQpcNs());
    void ResetHidTicks(uint64_t firstTick, uint64_t ticksPerSecond, int64_t localQpcNs = NowQpcNs());

    int64_t MapLocalQpcNs(int64_t qpcNs) const;
    int64_t MapMediaFoundationHns(int64_t timestampHns) const;
    int64_t MapAirPlayNtp(uint64_t ntpTimestamp) const;
    int64_t MapHidTicks(uint64_t tick) const;

    void RebaseTo(int64_t sourceTimestamp, int64_t localQpcNs, MediaTimestampDomain domain);
    MediaTimestampDomain Domain() const { return domain_; }

private:
    static int64_t NtpToNsSinceEra(uint64_t ntpTimestamp);

    MediaTimestampDomain domain_ = MediaTimestampDomain::Unknown;
    int64_t sourceBase_ = 0;
    int64_t localBaseQpcNs_ = 0;
    uint64_t hidTicksPerSecond_ = 0;
};

