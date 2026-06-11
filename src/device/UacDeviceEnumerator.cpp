#include "device/UacDeviceEnumerator.h"

#include "HResult.h"
#include "StringUtil.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <propsys.h>

namespace {

std::wstring GetDeviceId(IMMDevice* device)
{
    LPWSTR rawId = nullptr;
    const HRESULT hr = device->GetId(&rawId);
    if (FAILED(hr) || rawId == nullptr) {
        return {};
    }

    std::wstring id(rawId);
    CoTaskMemFree(rawId);
    return id;
}

std::wstring GetFriendlyName(IMMDevice* device)
{
    Microsoft::WRL::ComPtr<IPropertyStore> propertyStore;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
    if (FAILED(hr)) {
        LogHResult(L"IMMDevice::OpenPropertyStore", hr);
        return {};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &value);
    if (FAILED(hr)) {
        LogHResult(L"IPropertyStore::GetValue(PKEY_Device_FriendlyName)", hr);
        LOG_IF_FAILED(PropVariantClear(&value), L"PropVariantClear(friendly name failure)");
        return {};
    }

    std::wstring friendlyName;
    if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        friendlyName = value.pwszVal;
    }

    LOG_IF_FAILED(PropVariantClear(&value), L"PropVariantClear(friendly name)");
    return friendlyName;
}

} // namespace

HRESULT UacDeviceEnumerator::Enumerate(std::vector<UacDeviceInfo>& devices) const
{
    devices.clear();

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    RETURN_IF_FAILED_LOG(
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)),
        L"CoCreateInstance(MMDeviceEnumerator)");

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    RETURN_IF_FAILED_LOG(
        enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection),
        L"IMMDeviceEnumerator::EnumAudioEndpoints(eCapture)");

    UINT count = 0;
    RETURN_IF_FAILED_LOG(collection->GetCount(&count), L"IMMDeviceCollection::GetCount");

    for (UINT i = 0; i < count; ++i) {
        UacDeviceInfo info;
        HRESULT hr = collection->Item(i, &info.device);
        if (FAILED(hr)) {
            LogHResult(L"IMMDeviceCollection::Item", hr);
            continue;
        }

        info.deviceId = GetDeviceId(info.device.Get());
        info.friendlyName = GetFriendlyName(info.device.Get());

        Log::Write(L"UAC[%u]: name='%s', id='%s'",
            i,
            info.friendlyName.c_str(),
            info.deviceId.c_str());

        devices.push_back(info);
    }

    return S_OK;
}

HRESULT UacDeviceEnumerator::FindBestMatch(const std::wstring& match, UacDeviceInfo& selected) const
{
    std::vector<UacDeviceInfo> devices;
    RETURN_IF_FAILED_LOG(Enumerate(devices), L"UacDeviceEnumerator::Enumerate");

    if (devices.empty()) {
        Log::Write(L"No active UAC capture endpoints found.");
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    if (!match.empty()) {
        for (const auto& device : devices) {
            if (ContainsCaseInsensitive(device.friendlyName, match) ||
                ContainsCaseInsensitive(device.deviceId, match)) {
                selected = device;
                Log::Write(L"Selected UAC capture endpoint by match '%s': '%s'",
                    match.c_str(),
                    selected.friendlyName.c_str());
                return S_OK;
            }
        }

        Log::Write(L"No UAC endpoint matched '%s'.", match.c_str());
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    selected = devices.front();
    Log::Write(L"Selected first UAC capture endpoint: '%s'", selected.friendlyName.c_str());
    return S_OK;
}
