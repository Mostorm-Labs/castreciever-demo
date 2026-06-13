#pragma once

#include <cstdint>
#include <string>

enum class AirPlayPinMode {
    None,
    OnscreenPin,
    Password,
};

struct AirPlayFeatureSet {
    uint64_t bits = 0;

    static AirPlayFeatureSet DefaultMirrorH264V1(bool legacyPairing = true);
    void SetBit(int bit, bool enabled);
    bool HasBit(int bit) const;
    std::string ToTxtValue() const;
};

constexpr const char* kAirPlayModel = "AppleTV3,2";
constexpr const char* kAirPlaySourceVersion = "220.68";
constexpr const char* kAirPlayProtocolVersion = "2";
constexpr const char* kAirPlayPersistentId = "2e388006-13ba-4041-9a67-25dd4a43d536";

