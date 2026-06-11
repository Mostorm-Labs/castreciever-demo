#include "device/UvcDeviceEnumerator.h"

#include "HResult.h"
#include "StringUtil.h"

#include <mfapi.h>

namespace {

std::wstring GetAllocatedAttributeString(IMFAttributes* attributes, REFGUID key)
{
    wchar_t* rawValue = nullptr;
    UINT32 length = 0;
    const HRESULT hr = attributes->GetAllocatedString(key, &rawValue, &length);
    if (FAILED(hr) || rawValue == nullptr) {
        return {};
    }

    std::wstring value(rawValue, length);
    CoTaskMemFree(rawValue);
    return value;
}

} // namespace

HRESULT UvcDeviceEnumerator::Enumerate(std::vector<UvcDeviceInfo>& devices) const
{
    devices.clear();

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    RETURN_IF_FAILED_LOG(MFCreateAttributes(&attributes, 1), L"MFCreateAttributes(UVC)");
    RETURN_IF_FAILED_LOG(
        attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
        L"IMFAttributes::SetGUID(UVC source type)");

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attributes.Get(), &activates, &count);
    if (FAILED(hr)) {
        LogHResult(L"MFEnumDeviceSources(UVC)", hr);
        return hr;
    }

    for (UINT32 i = 0; i < count; ++i) {
        UvcDeviceInfo info;
        info.activate = activates[i];
        info.friendlyName = GetAllocatedAttributeString(activates[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        info.symbolicLink = GetAllocatedAttributeString(activates[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);

        Log::Write(L"UVC[%u]: name='%s', link='%s'",
            i,
            info.friendlyName.c_str(),
            info.symbolicLink.c_str());

        devices.push_back(info);
        activates[i]->Release();
    }

    CoTaskMemFree(activates);
    return S_OK;
}

HRESULT UvcDeviceEnumerator::FindBestMatch(const std::wstring& match, UvcDeviceInfo& selected) const
{
    std::vector<UvcDeviceInfo> devices;
    RETURN_IF_FAILED_LOG(Enumerate(devices), L"UvcDeviceEnumerator::Enumerate");

    if (devices.empty()) {
        Log::Write(L"No UVC video devices found.");
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    if (!match.empty()) {
        for (const auto& device : devices) {
            if (ContainsCaseInsensitive(device.friendlyName, match) ||
                ContainsCaseInsensitive(device.symbolicLink, match)) {
                selected = device;
                Log::Write(L"Selected UVC device by match '%s': '%s'",
                    match.c_str(),
                    selected.friendlyName.c_str());
                return S_OK;
            }
        }

        Log::Write(L"No UVC device matched '%s'.", match.c_str());
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    selected = devices.front();
    Log::Write(L"Selected first UVC device: '%s'", selected.friendlyName.c_str());
    return S_OK;
}
