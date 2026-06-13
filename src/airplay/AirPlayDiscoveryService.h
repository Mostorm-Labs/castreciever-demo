#pragma once

#include "airplay/AirPlayDeviceId.h"
#include "airplay/AirPlayFeatures.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

struct AirPlayDiscoveryOptions {
    std::wstring name = L"UsbCastReceiver";
    std::array<uint8_t, 6> deviceId = {};
    uint16_t airplayPort = 7000;
    uint16_t raopPort = 7000;
    AirPlayFeatureSet features = AirPlayFeatureSet::DefaultMirrorH264V1();
    AirPlayPinMode pinMode = AirPlayPinMode::None;
    std::string publicKeyHex;
};

class AirPlayDiscoveryService {
public:
    AirPlayDiscoveryService();
    ~AirPlayDiscoveryService();

    HRESULT Start(const AirPlayDiscoveryOptions& options);
    void Stop();
    HRESULT UpdateFeatures(const AirPlayFeatureSet& features);

    bool IsRunning() const { return running_; }

private:
    struct Impl;

    HRESULT EnsureLoaded();
    HRESULT RegisterRaop();
    HRESULT RegisterAirPlay();
    void ReleaseRegistrations();
    void ReleaseLibrary();

    Impl* impl_ = nullptr;
    AirPlayDiscoveryOptions options_;
    bool running_ = false;
};

