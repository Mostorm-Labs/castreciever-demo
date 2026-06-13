#include "airplay/AirPlayDeviceId.h"

#include "HResult.h"
#include "Log.h"

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kStorageDirectoryName[] = L"UsbCastReceiver";
constexpr wchar_t kDeviceIdFileName[] = L"airplay-device-id.bin";
constexpr wchar_t kDiscoveryPkFileName[] = L"airplay-discovery-pk.bin";

std::wstring LocalAppDataDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return {};
    }
    return path;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty() || DirectoryExists(path)) {
        return true;
    }

    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        EnsureDirectory(path.substr(0, slash));
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }

    const DWORD error = GetLastError();
    return error == ERROR_ALREADY_EXISTS && DirectoryExists(path);
}

std::wstring StorageDirectory()
{
    const std::wstring localAppData = LocalAppDataDirectory();
    if (localAppData.empty()) {
        return {};
    }
    return localAppData + L"\\" + kStorageDirectoryName;
}

std::wstring StorageFilePath(const wchar_t* fileName)
{
    const std::wstring directory = StorageDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory + L"\\" + fileName;
}

bool ReadExactFile(const std::wstring& path, uint8_t* data, DWORD byteCount)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD read = 0;
    const BOOL ok = ReadFile(file, data, byteCount, &read, nullptr);
    CloseHandle(file);
    return ok && read == byteCount;
}

bool WriteExactFile(const std::wstring& path, const uint8_t* data, DWORD byteCount)
{
    const std::wstring directory = StorageDirectory();
    if (!directory.empty()) {
        EnsureDirectory(directory);
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, byteCount, &written, nullptr);
    CloseHandle(file);
    return ok && written == byteCount;
}

bool FillRandom(uint8_t* data, ULONG byteCount)
{
    const NTSTATUS status = BCryptGenRandom(nullptr, data, byteCount, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status >= 0) {
        return true;
    }

    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    for (ULONG i = 0; i < byteCount; ++i) {
        counter.QuadPart = counter.QuadPart * 1103515245 + 12345 + i;
        data[i] = static_cast<uint8_t>((counter.QuadPart >> 16) & 0xFF);
    }
    return false;
}

std::vector<uint8_t> ResolvePersistentRandomBytes(const wchar_t* fileName, size_t byteCount)
{
    std::vector<uint8_t> bytes(byteCount);
    const std::wstring path = StorageFilePath(fileName);
    if (!path.empty() && ReadExactFile(path, bytes.data(), static_cast<DWORD>(bytes.size()))) {
        return bytes;
    }

    const bool strongRandom = FillRandom(bytes.data(), static_cast<ULONG>(bytes.size()));
    if (!path.empty() && !WriteExactFile(path, bytes.data(), static_cast<DWORD>(bytes.size()))) {
        Log::Write(L"AirPlay identity persistence failed for '%s'; using in-memory random identity this run.", path.c_str());
    }
    if (!strongRandom) {
        Log::Write(L"BCryptGenRandom unavailable for AirPlay identity; used QPC fallback.");
    }
    return bytes;
}

bool IsUsableAdapter(const IP_ADAPTER_ADDRESSES* adapter)
{
    if (adapter == nullptr) {
        return false;
    }
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
        return false;
    }
    if (adapter->OperStatus != IfOperStatusUp) {
        return false;
    }
    return adapter->PhysicalAddressLength == 6;
}

bool TryResolveAdapterMac(std::array<uint8_t, 6>& deviceId)
{
    ULONG bufferSize = 15 * 1024;
    std::vector<uint8_t> buffer(bufferSize);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize);
    }

    if (result != NO_ERROR) {
        Log::Write(L"GetAdaptersAddresses failed while resolving AirPlay device id: %lu", result);
        return false;
    }

    for (const IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (!IsUsableAdapter(adapter)) {
            continue;
        }

        std::copy(adapter->PhysicalAddress, adapter->PhysicalAddress + 6, deviceId.begin());
        return true;
    }

    return false;
}

std::string BytesToHex(const uint8_t* data, size_t size, bool uppercase, bool colon)
{
    const char* format = uppercase ? "%02X" : "%02x";
    std::string result;
    char byteText[4] = {};

    for (size_t i = 0; i < size; ++i) {
        if (colon && i != 0) {
            result.push_back(':');
        }
        std::snprintf(byteText, sizeof(byteText), format, data[i]);
        result.append(byteText);
    }

    return result;
}

} // namespace

AirPlayDeviceIdentity ResolveAirPlayDeviceIdentity()
{
    AirPlayDeviceIdentity identity;
    if (TryResolveAdapterMac(identity.deviceId)) {
        identity.fromNetworkAdapter = true;
        return identity;
    }

    const std::vector<uint8_t> persisted = ResolvePersistentRandomBytes(kDeviceIdFileName, identity.deviceId.size());
    std::copy(persisted.begin(), persisted.end(), identity.deviceId.begin());
    identity.fromNetworkAdapter = false;
    return identity;
}

std::string FormatAirPlayDeviceId(const std::array<uint8_t, 6>& deviceId)
{
    return BytesToHex(deviceId.data(), deviceId.size(), false, true);
}

std::string FormatRaopDeviceId(const std::array<uint8_t, 6>& deviceId)
{
    return BytesToHex(deviceId.data(), deviceId.size(), true, false);
}

std::string ResolvePersistentDiscoveryPublicKeyHex()
{
    const std::vector<uint8_t> bytes = ResolvePersistentRandomBytes(kDiscoveryPkFileName, 32);
    return BytesToHex(bytes.data(), bytes.size(), false, false);
}

