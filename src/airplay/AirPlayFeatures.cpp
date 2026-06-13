#include "airplay/AirPlayFeatures.h"

#include <cstdio>

AirPlayFeatureSet AirPlayFeatureSet::DefaultMirrorH264V1(bool legacyPairing)
{
    AirPlayFeatureSet features;

    features.SetBit(0, false); // HLS video playback intentionally disabled in v1.
    features.SetBit(1, true);
    features.SetBit(2, true);
    features.SetBit(3, false);
    features.SetBit(4, false); // HLS intentionally disabled in v1.
    features.SetBit(5, true);
    features.SetBit(6, true);
    features.SetBit(7, true);  // Mirroring supported.
    features.SetBit(8, false);
    features.SetBit(9, true);  // Audio supported.
    features.SetBit(10, true);
    features.SetBit(11, true); // Audio packet redundancy.
    features.SetBit(12, true); // FairPlay secure auth.
    features.SetBit(13, true);
    features.SetBit(14, true);
    features.SetBit(15, true);
    features.SetBit(16, true);
    features.SetBit(17, true);
    features.SetBit(18, true);
    features.SetBit(19, true);
    features.SetBit(20, true);
    features.SetBit(21, true);
    features.SetBit(22, true);
    features.SetBit(23, false);
    features.SetBit(24, false);
    features.SetBit(25, true);
    features.SetBit(26, false);
    features.SetBit(27, legacyPairing);
    features.SetBit(28, true);
    features.SetBit(29, false);
    features.SetBit(30, true); // RAOP support.
    features.SetBit(31, false);
    features.SetBit(42, false); // HEVC/multi-codec intentionally disabled in v1.

    return features;
}

void AirPlayFeatureSet::SetBit(int bit, bool enabled)
{
    if (bit < 0 || bit > 63) {
        return;
    }

    const uint64_t mask = 1ULL << static_cast<uint64_t>(bit);
    if (enabled) {
        bits |= mask;
    } else {
        bits &= ~mask;
    }
}

bool AirPlayFeatureSet::HasBit(int bit) const
{
    if (bit < 0 || bit > 63) {
        return false;
    }

    return (bits & (1ULL << static_cast<uint64_t>(bit))) != 0;
}

std::string AirPlayFeatureSet::ToTxtValue() const
{
    const uint32_t low = static_cast<uint32_t>(bits & 0xFFFFFFFFULL);
    const uint32_t high = static_cast<uint32_t>((bits >> 32) & 0xFFFFFFFFULL);

    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%X,0x%X", low, high);
    return buffer;
}

