#pragma once

#include <windows.h>

#include <cwctype>
#include <string>

inline std::wstring ToLowerCopy(std::wstring value)
{
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

inline bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& needle)
{
    if (needle.empty()) {
        return true;
    }

    const std::wstring loweredValue = ToLowerCopy(value);
    const std::wstring loweredNeedle = ToLowerCopy(needle);
    return loweredValue.find(loweredNeedle) != std::wstring::npos;
}

inline std::wstring GuidToString(REFGUID guid)
{
    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, static_cast<int>(ARRAYSIZE(buffer)));
    return buffer;
}
