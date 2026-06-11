#pragma once

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

struct UacDeviceInfo {
    std::wstring friendlyName;
    std::wstring deviceId;
    Microsoft::WRL::ComPtr<IMMDevice> device;
};

class UacDeviceEnumerator {
public:
    HRESULT Enumerate(std::vector<UacDeviceInfo>& devices) const;
    HRESULT FindBestMatch(const std::wstring& match, UacDeviceInfo& selected) const;
};
