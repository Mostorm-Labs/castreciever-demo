#include "airplay/AirPlayDiscoveryService.h"

#include "HResult.h"
#include "Log.h"

#include <strsafe.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kRaopTxtVers = "1";
constexpr const char* kRaopChannels = "2";
constexpr const char* kRaopCodecs = "0,1,2,3";
constexpr const char* kRaopEncryption = "0,3,5";
constexpr const char* kRaopProtocolVersion = "2";
constexpr const char* kRaopRhd = "5.6.0.0";
constexpr const char* kRaopSampleRate = "44100";
constexpr const char* kRaopSampleSize = "16";
constexpr const char* kRaopTransport = "UDP";
constexpr const char* kRaopMetadata = "0,1,2";
constexpr const char* kRaopVn = "65537";

using DNSServiceRef = struct DNSServiceRefOpaque*;
using DNSServiceFlags = uint32_t;
using DNSServiceErrorType = int32_t;

union TXTRecordRef {
    char PrivateData[16];
    char* ForceNaturalAlignment;
};

using DNSServiceRegisterReply = void(__stdcall*)(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    DNSServiceErrorType errorCode,
    const char* name,
    const char* regtype,
    const char* domain,
    void* context);

using DNSServiceRegister_t = DNSServiceErrorType(__stdcall*)(
    DNSServiceRef* sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    const char* name,
    const char* regtype,
    const char* domain,
    const char* host,
    uint16_t port,
    uint16_t txtLen,
    const void* txtRecord,
    DNSServiceRegisterReply callBack,
    void* context);

using DNSServiceRefDeallocate_t = void(__stdcall*)(DNSServiceRef sdRef);
using TXTRecordCreate_t = void(__stdcall*)(TXTRecordRef* txtRecord, uint16_t bufferLen, void* buffer);
using TXTRecordDeallocate_t = void(__stdcall*)(TXTRecordRef* txtRecord);
using TXTRecordSetValue_t = DNSServiceErrorType(__stdcall*)(TXTRecordRef* txtRecord, const char* key, uint8_t valueSize, const void* value);
using TXTRecordGetLength_t = uint16_t(__stdcall*)(const TXTRecordRef* txtRecord);
using TXTRecordGetBytesPtr_t = const void*(__stdcall*)(const TXTRecordRef* txtRecord);

uint16_t HostToNetworkPort(uint16_t port)
{
    return static_cast<uint16_t>(((port & 0x00FFU) << 8) | ((port & 0xFF00U) >> 8));
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
    if (!result.empty()) {
        result.pop_back();
    }
    return result;
}

HRESULT HResultFromDnsSd(DNSServiceErrorType error)
{
    if (error == 0) {
        return S_OK;
    }
    return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, static_cast<USHORT>(-error));
}

void __stdcall RegisterReply(
    DNSServiceRef,
    DNSServiceFlags,
    DNSServiceErrorType errorCode,
    const char* name,
    const char* regtype,
    const char*,
    void*)
{
    if (errorCode == 0) {
        Log::Write(L"AirPlay DNS-SD service registered: name='%S' type='%S'", name ? name : "", regtype ? regtype : "");
    } else {
        Log::Write(L"AirPlay DNS-SD registration callback error=%d name='%S' type='%S'", errorCode, name ? name : "", regtype ? regtype : "");
    }
}

std::string PinPwValue(AirPlayPinMode pinMode)
{
    return pinMode == AirPlayPinMode::None ? "false" : "true";
}

std::string AirPlayFlagsValue(AirPlayPinMode)
{
    return "0x4";
}

std::string RaopSfValue(AirPlayPinMode pinMode)
{
    if (pinMode == AirPlayPinMode::OnscreenPin) {
        return "0x8c";
    }
    if (pinMode == AirPlayPinMode::Password) {
        return "0x84";
    }
    return "0x4";
}

} // namespace

struct AirPlayDiscoveryService::Impl {
    HMODULE module = nullptr;
    DNSServiceRegister_t DNSServiceRegister = nullptr;
    DNSServiceRefDeallocate_t DNSServiceRefDeallocate = nullptr;
    TXTRecordCreate_t TXTRecordCreate = nullptr;
    TXTRecordSetValue_t TXTRecordSetValue = nullptr;
    TXTRecordGetLength_t TXTRecordGetLength = nullptr;
    TXTRecordGetBytesPtr_t TXTRecordGetBytesPtr = nullptr;
    TXTRecordDeallocate_t TXTRecordDeallocate = nullptr;

    TXTRecordRef raopRecord = {};
    TXTRecordRef airplayRecord = {};
    bool raopRecordCreated = false;
    bool airplayRecordCreated = false;
    DNSServiceRef raopService = nullptr;
    DNSServiceRef airplayService = nullptr;
};

AirPlayDiscoveryService::AirPlayDiscoveryService()
    : impl_(new Impl())
{
}

AirPlayDiscoveryService::~AirPlayDiscoveryService()
{
    Stop();
    delete impl_;
}

HRESULT AirPlayDiscoveryService::Start(const AirPlayDiscoveryOptions& options)
{
    Stop();
    options_ = options;
    if (options_.name.empty()) {
        options_.name = L"UsbCastReceiver";
    }
    if (options_.publicKeyHex.empty()) {
        options_.publicKeyHex = ResolvePersistentDiscoveryPublicKeyHex();
    }

    HRESULT hr = EnsureLoaded();
    if (FAILED(hr)) {
        LogHResult(L"AirPlayDiscoveryService::EnsureLoaded", hr);
        Stop();
        return hr;
    }

    hr = RegisterRaop();
    if (FAILED(hr)) {
        LogHResult(L"AirPlayDiscoveryService::RegisterRaop", hr);
        Stop();
        return hr;
    }

    hr = RegisterAirPlay();
    if (FAILED(hr)) {
        LogHResult(L"AirPlayDiscoveryService::RegisterAirPlay", hr);
        Stop();
        return hr;
    }

    running_ = true;
    Log::Write(
        L"AirPlay discovery started. name='%s' deviceid='%S' features='%S' airplay-port=%u raop-port=%u",
        options_.name.c_str(),
        FormatAirPlayDeviceId(options_.deviceId).c_str(),
        options_.features.ToTxtValue().c_str(),
        options_.airplayPort,
        options_.raopPort);
    return S_OK;
}

void AirPlayDiscoveryService::Stop()
{
    ReleaseRegistrations();
    ReleaseLibrary();
    running_ = false;
}

HRESULT AirPlayDiscoveryService::UpdateFeatures(const AirPlayFeatureSet& features)
{
    options_.features = features;
    if (!running_) {
        return S_OK;
    }

    AirPlayDiscoveryOptions restartOptions = options_;
    Stop();
    return Start(restartOptions);
}

HRESULT AirPlayDiscoveryService::EnsureLoaded()
{
    if (impl_->module != nullptr) {
        return S_OK;
    }

    impl_->module = LoadLibraryA("dnssd.dll");
    if (impl_->module == nullptr) {
        const HRESULT hr = HResultFromLastError();
        Log::Write(L"AirPlay discovery disabled: dnssd.dll could not be loaded. Bonjour is probably not installed.");
        return hr;
    }

    impl_->DNSServiceRegister = reinterpret_cast<DNSServiceRegister_t>(GetProcAddress(impl_->module, "DNSServiceRegister"));
    impl_->DNSServiceRefDeallocate = reinterpret_cast<DNSServiceRefDeallocate_t>(GetProcAddress(impl_->module, "DNSServiceRefDeallocate"));
    impl_->TXTRecordCreate = reinterpret_cast<TXTRecordCreate_t>(GetProcAddress(impl_->module, "TXTRecordCreate"));
    impl_->TXTRecordSetValue = reinterpret_cast<TXTRecordSetValue_t>(GetProcAddress(impl_->module, "TXTRecordSetValue"));
    impl_->TXTRecordGetLength = reinterpret_cast<TXTRecordGetLength_t>(GetProcAddress(impl_->module, "TXTRecordGetLength"));
    impl_->TXTRecordGetBytesPtr = reinterpret_cast<TXTRecordGetBytesPtr_t>(GetProcAddress(impl_->module, "TXTRecordGetBytesPtr"));
    impl_->TXTRecordDeallocate = reinterpret_cast<TXTRecordDeallocate_t>(GetProcAddress(impl_->module, "TXTRecordDeallocate"));

    if (!impl_->DNSServiceRegister ||
        !impl_->DNSServiceRefDeallocate ||
        !impl_->TXTRecordCreate ||
        !impl_->TXTRecordSetValue ||
        !impl_->TXTRecordGetLength ||
        !impl_->TXTRecordGetBytesPtr ||
        !impl_->TXTRecordDeallocate) {
        Log::Write(L"AirPlay discovery disabled: dnssd.dll is missing required DNS-SD exports.");
        ReleaseLibrary();
        return E_NOINTERFACE;
    }

    return S_OK;
}

HRESULT AirPlayDiscoveryService::RegisterRaop()
{
    const std::string features = options_.features.ToTxtValue();
    const std::string pw = PinPwValue(options_.pinMode);
    const std::string sf = RaopSfValue(options_.pinMode);
    const std::string raopId = FormatRaopDeviceId(options_.deviceId);
    const std::string name = WideToUtf8(options_.name);
    const std::string serviceName = raopId + "@" + name;

    impl_->TXTRecordCreate(&impl_->raopRecord, 0, nullptr);
    impl_->raopRecordCreated = true;

    auto add = [this](const char* key, const std::string& value) -> HRESULT {
        const DNSServiceErrorType error = impl_->TXTRecordSetValue(
            &impl_->raopRecord,
            key,
            static_cast<uint8_t>(std::min<size_t>(value.size(), 255)),
            value.data());
        if (error != 0) {
            Log::Write(L"RAOP TXTRecordSetValue failed key='%S' error=%d", key, error);
            return HResultFromDnsSd(error);
        }
        return S_OK;
    };

    RETURN_IF_FAILED_LOG(add("ch", kRaopChannels), L"RAOP TXT ch");
    RETURN_IF_FAILED_LOG(add("cn", kRaopCodecs), L"RAOP TXT cn");
    RETURN_IF_FAILED_LOG(add("da", "true"), L"RAOP TXT da");
    RETURN_IF_FAILED_LOG(add("et", kRaopEncryption), L"RAOP TXT et");
    RETURN_IF_FAILED_LOG(add("vv", kRaopProtocolVersion), L"RAOP TXT vv");
    RETURN_IF_FAILED_LOG(add("ft", features), L"RAOP TXT ft");
    RETURN_IF_FAILED_LOG(add("am", kAirPlayModel), L"RAOP TXT am");
    RETURN_IF_FAILED_LOG(add("md", kRaopMetadata), L"RAOP TXT md");
    RETURN_IF_FAILED_LOG(add("rhd", kRaopRhd), L"RAOP TXT rhd");
    RETURN_IF_FAILED_LOG(add("pw", pw), L"RAOP TXT pw");
    RETURN_IF_FAILED_LOG(add("sf", sf), L"RAOP TXT sf");
    RETURN_IF_FAILED_LOG(add("sr", kRaopSampleRate), L"RAOP TXT sr");
    RETURN_IF_FAILED_LOG(add("ss", kRaopSampleSize), L"RAOP TXT ss");
    RETURN_IF_FAILED_LOG(add("sv", "false"), L"RAOP TXT sv");
    RETURN_IF_FAILED_LOG(add("tp", kRaopTransport), L"RAOP TXT tp");
    RETURN_IF_FAILED_LOG(add("txtvers", kRaopTxtVers), L"RAOP TXT txtvers");
    RETURN_IF_FAILED_LOG(add("vs", kAirPlaySourceVersion), L"RAOP TXT vs");
    RETURN_IF_FAILED_LOG(add("vn", kRaopVn), L"RAOP TXT vn");
    RETURN_IF_FAILED_LOG(add("pk", options_.publicKeyHex), L"RAOP TXT pk");

    const DNSServiceErrorType error = impl_->DNSServiceRegister(
        &impl_->raopService,
        0,
        0,
        serviceName.c_str(),
        "_raop._tcp",
        nullptr,
        nullptr,
        HostToNetworkPort(options_.raopPort),
        impl_->TXTRecordGetLength(&impl_->raopRecord),
        impl_->TXTRecordGetBytesPtr(&impl_->raopRecord),
        RegisterReply,
        this);
    if (error != 0) {
        Log::Write(L"DNSServiceRegister(_raop._tcp) failed: error=%d", error);
        return HResultFromDnsSd(error);
    }

    return S_OK;
}

HRESULT AirPlayDiscoveryService::RegisterAirPlay()
{
    const std::string name = WideToUtf8(options_.name);
    const std::string deviceId = FormatAirPlayDeviceId(options_.deviceId);
    const std::string features = options_.features.ToTxtValue();
    const std::string pw = PinPwValue(options_.pinMode);
    const std::string flags = AirPlayFlagsValue(options_.pinMode);

    impl_->TXTRecordCreate(&impl_->airplayRecord, 0, nullptr);
    impl_->airplayRecordCreated = true;

    auto add = [this](const char* key, const std::string& value) -> HRESULT {
        const DNSServiceErrorType error = impl_->TXTRecordSetValue(
            &impl_->airplayRecord,
            key,
            static_cast<uint8_t>(std::min<size_t>(value.size(), 255)),
            value.data());
        if (error != 0) {
            Log::Write(L"AirPlay TXTRecordSetValue failed key='%S' error=%d", key, error);
            return HResultFromDnsSd(error);
        }
        return S_OK;
    };

    RETURN_IF_FAILED_LOG(add("deviceid", deviceId), L"AirPlay TXT deviceid");
    RETURN_IF_FAILED_LOG(add("features", features), L"AirPlay TXT features");
    RETURN_IF_FAILED_LOG(add("pw", pw), L"AirPlay TXT pw");
    RETURN_IF_FAILED_LOG(add("flags", flags), L"AirPlay TXT flags");
    RETURN_IF_FAILED_LOG(add("model", kAirPlayModel), L"AirPlay TXT model");
    RETURN_IF_FAILED_LOG(add("pk", options_.publicKeyHex), L"AirPlay TXT pk");
    RETURN_IF_FAILED_LOG(add("pi", kAirPlayPersistentId), L"AirPlay TXT pi");
    RETURN_IF_FAILED_LOG(add("srcvers", kAirPlaySourceVersion), L"AirPlay TXT srcvers");
    RETURN_IF_FAILED_LOG(add("vv", kAirPlayProtocolVersion), L"AirPlay TXT vv");

    const DNSServiceErrorType error = impl_->DNSServiceRegister(
        &impl_->airplayService,
        0,
        0,
        name.c_str(),
        "_airplay._tcp",
        nullptr,
        nullptr,
        HostToNetworkPort(options_.airplayPort),
        impl_->TXTRecordGetLength(&impl_->airplayRecord),
        impl_->TXTRecordGetBytesPtr(&impl_->airplayRecord),
        RegisterReply,
        this);
    if (error != 0) {
        Log::Write(L"DNSServiceRegister(_airplay._tcp) failed: error=%d", error);
        return HResultFromDnsSd(error);
    }

    return S_OK;
}

void AirPlayDiscoveryService::ReleaseRegistrations()
{
    if (impl_ == nullptr) {
        return;
    }

    if (impl_->raopService != nullptr && impl_->DNSServiceRefDeallocate != nullptr) {
        impl_->DNSServiceRefDeallocate(impl_->raopService);
        impl_->raopService = nullptr;
    }
    if (impl_->airplayService != nullptr && impl_->DNSServiceRefDeallocate != nullptr) {
        impl_->DNSServiceRefDeallocate(impl_->airplayService);
        impl_->airplayService = nullptr;
    }
    if (impl_->raopRecordCreated && impl_->TXTRecordDeallocate != nullptr) {
        impl_->TXTRecordDeallocate(&impl_->raopRecord);
        impl_->raopRecordCreated = false;
    }
    if (impl_->airplayRecordCreated && impl_->TXTRecordDeallocate != nullptr) {
        impl_->TXTRecordDeallocate(&impl_->airplayRecord);
        impl_->airplayRecordCreated = false;
    }
}

void AirPlayDiscoveryService::ReleaseLibrary()
{
    if (impl_ == nullptr || impl_->module == nullptr) {
        return;
    }

    FreeLibrary(impl_->module);
    impl_->module = nullptr;
    impl_->DNSServiceRegister = nullptr;
    impl_->DNSServiceRefDeallocate = nullptr;
    impl_->TXTRecordCreate = nullptr;
    impl_->TXTRecordSetValue = nullptr;
    impl_->TXTRecordGetLength = nullptr;
    impl_->TXTRecordGetBytesPtr = nullptr;
    impl_->TXTRecordDeallocate = nullptr;
}
