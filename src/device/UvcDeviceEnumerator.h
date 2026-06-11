#pragma once

#include <mfidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>

struct UvcDeviceInfo {
    std::wstring friendlyName;
    std::wstring symbolicLink;
    Microsoft::WRL::ComPtr<IMFActivate> activate;
};

class UvcDeviceEnumerator {
public:
    HRESULT Enumerate(std::vector<UvcDeviceInfo>& devices) const;
    HRESULT FindBestMatch(const std::wstring& match, UvcDeviceInfo& selected) const;
};
