#pragma once

#include <array>
#include <cstdint>
#include <string>

struct AirPlayDeviceIdentity {
    std::array<uint8_t, 6> deviceId = {};
    bool fromNetworkAdapter = false;
};

AirPlayDeviceIdentity ResolveAirPlayDeviceIdentity();
std::string FormatAirPlayDeviceId(const std::array<uint8_t, 6>& deviceId);
std::string FormatRaopDeviceId(const std::array<uint8_t, 6>& deviceId);
std::string ResolvePersistentDiscoveryPublicKeyHex();
